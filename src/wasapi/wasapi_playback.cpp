#include "wasapi/wasapi_playback.h"
#include "audio/dsd_packer.h"

#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace wasio {
namespace {

constexpr std::uint8_t kDopMarkerEven = 0x05;
constexpr std::uint8_t kDopMarkerOdd = 0xfa;
constexpr std::uint8_t kDopSilenceByte = 0x55;
constexpr std::uint8_t kNativeSilenceByte = 0xaa;

std::int32_t read_source_sample(const std::uint8_t* source, PcmEncoding encoding)
{
    switch (encoding) {
    case PcmEncoding::Int16: {
        const auto value = static_cast<std::int16_t>(source[0] | (source[1] << 8));
        return static_cast<std::int32_t>(
            static_cast<std::uint32_t>(static_cast<std::int32_t>(value)) << 16);
    }
    case PcmEncoding::Int24: {
        std::int32_t value = source[0] | (source[1] << 8) | (source[2] << 16);
        if ((value & 0x00800000) != 0) value |= static_cast<std::int32_t>(0xff000000);
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << 8);
    }
    case PcmEncoding::Int32:
        return static_cast<std::int32_t>(source[0] | (source[1] << 8) | (source[2] << 16) |
                                         (source[3] << 24));
    }
    return 0;
}

// WASAPI containers are little-endian and, when wValidBitsPerSample is smaller
// than wBitsPerSample, the payload sits in the most significant bits.
void write_container_sample(std::uint8_t* destination, std::size_t container_bytes,
                            std::uint16_t valid_bits, std::int32_t value)
{
    auto bits = static_cast<std::uint32_t>(value);
    switch (container_bytes) {
    case 2:
        destination[0] = static_cast<std::uint8_t>(bits >> 16);
        destination[1] = static_cast<std::uint8_t>(bits >> 24);
        break;
    case 3:
        destination[0] = static_cast<std::uint8_t>(bits >> 8);
        destination[1] = static_cast<std::uint8_t>(bits >> 16);
        destination[2] = static_cast<std::uint8_t>(bits >> 24);
        break;
    case 4:
    default:
        if (valid_bits == 24) bits &= 0xffffff00u;
        destination[0] = static_cast<std::uint8_t>(bits);
        destination[1] = static_cast<std::uint8_t>(bits >> 8);
        destination[2] = static_cast<std::uint8_t>(bits >> 16);
        destination[3] = static_cast<std::uint8_t>(bits >> 24);
        break;
    }
}

} // namespace

WasapiPlayback::~WasapiPlayback()
{
    stop();
}

bool WasapiPlayback::start(WasapiPlaybackConfig config, std::string* error_message)
{
    stop();
    render_error_.clear();
    start_failure_ = EngineStartFailure::DeviceOpen;
    mode_ = config.mode;
    if (mode_ == WasapiOutputMode::Pcm) {
        if (!config.pcm_source || !config.pcm_source->format().valid()) {
            if (error_message) *error_message = "PCM source is missing or invalid";
            return false;
        }
        pcm_source_ = std::move(config.pcm_source);
    } else {
        if (!config.dsd_source || !config.dsd_source->format().valid()) {
            if (error_message) *error_message = "DSD source is missing or invalid";
            return false;
        }
        dsd_source_ = std::move(config.dsd_source);
        dsd_sample_rate_ = dsd_source_->format().sample_rate;
    }
    if (config.ring_frames < 2) {
        if (error_message) *error_message = "Ring configuration is invalid";
        return false;
    }

    if (!session_.open(config.endpoint_name, error_message)) {
        pcm_source_.reset();
        dsd_source_.reset();
        return false;
    }
    endpoint_name_ = session_.endpoint().name;
    if (!configure_output(error_message)) {
        stop();
        return false;
    }

    const std::size_t ring_frames =
        std::max<std::size_t>(config.ring_frames, static_cast<std::size_t>(buffer_frame_count_) * 8);
    ring_ = std::make_unique<SpscByteRing>(ring_frames * output_frame_bytes_);
    worker_frames_ = 1024;
    if (mode_ == WasapiOutputMode::Pcm) {
        source_scratch_.resize(worker_frames_ * pcm_source_->format().bytes_per_frame());
    } else {
        source_scratch_.resize(worker_frames_ * dsd_source_->format().bytes_per_frame() *
                               source_frames_per_output_);
    }
    conversion_scratch_.resize(worker_frames_ * output_frame_bytes_);

    stop_worker_.store(false, std::memory_order_release);
    stop_render_.store(false, std::memory_order_release);
    source_finished_.store(false, std::memory_order_release);
    playback_finished_.store(false, std::memory_order_release);
    underruns_.store(0, std::memory_order_relaxed);
    render_callbacks_.store(0, std::memory_order_relaxed);
    render_state_.store(0, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    seek_state_.store(SeekState::None, std::memory_order_release);
    seek_target_.store(0, std::memory_order_relaxed);
    pending_output_position_.store(0, std::memory_order_relaxed);
    output_position_frames_.store(0, std::memory_order_relaxed);
    total_output_frames_ = mode_ == WasapiOutputMode::Pcm
                               ? pcm_source_->total_frames()
                               : dsd_source_->total_frames() / source_frames_per_output_;
    render_output_frames_ = 0;
    source_worker_ = std::thread(&WasapiPlayback::run_source_worker, this);

    // Prime several periods on the control thread before the render thread
    // touches the device; the render thread never waits on the source.
    const std::size_t prime_bytes =
        static_cast<std::size_t>(buffer_frame_count_) * output_frame_bytes_ * 2;
    for (int attempt = 0; attempt < 500 && ring_->available_to_read() < prime_bytes &&
                          !source_finished_.load(std::memory_order_acquire);
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    render_thread_ = std::thread(&WasapiPlayback::run_render_thread, this);
    for (int attempt = 0; attempt < 3000 && render_state_.load(std::memory_order_acquire) == 0;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (render_state_.load(std::memory_order_acquire) != 1) {
        const std::string reason =
            render_error_.empty() ? std::string("the WASAPI render thread did not start")
                                  : render_error_;
        if (error_message) *error_message = reason;
        stop();
        return false;
    }
    start_failure_ = EngineStartFailure::None;
    running_.store(true, std::memory_order_release);
    return true;
}

void WasapiPlayback::stop()
{
    stop_render_.store(true, std::memory_order_release);
    if (session_.event_handle() != nullptr) SetEvent(session_.event_handle());
    if (render_thread_.joinable()) render_thread_.join();
    stop_worker_.store(true, std::memory_order_release);
    if (source_worker_.joinable()) source_worker_.join();
    running_.store(false, std::memory_order_release);
    playback_finished_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    seek_state_.store(SeekState::None, std::memory_order_release);
    session_.stop();
    session_.close();
    ring_.reset();
    pcm_source_.reset();
    dsd_source_.reset();
    source_scratch_.clear();
    conversion_scratch_.clear();
    buffer_frame_count_ = 0;
}

bool WasapiPlayback::configure_output(std::string* error_message)
{
    WasapiFormatRequest request;
    request.mode = mode_;
    std::size_t source_channels = 0;
    switch (mode_) {
    case WasapiOutputMode::Pcm: {
        const auto& format = pcm_source_->format();
        source_channels = format.channels;
        output_sample_rate_ = format.sample_rate;
        source_frames_per_output_ = 1;
        break;
    }
    case WasapiOutputMode::DoP: {
        const auto rate = dsd_source_->format().sample_rate;
        source_channels = dsd_source_->format().channels;
        if (rate % 16 != 0) {
            start_failure_ = EngineStartFailure::FormatNegotiation;
            if (error_message) *error_message = "DSD rate cannot be represented by DoP";
            return false;
        }
        if (rate > kMaxDopDsdRate) {
            if (error_message) {
                *error_message =
                    "DoP512 is not supported: it requires a 1,411,200 Hz PCM host rate";
            }
            return false;
        }
        output_sample_rate_ = static_cast<double>(rate / 16);
        source_frames_per_output_ = 2;
        break;
    }
    // Unreachable through the shipped players; see WasapiOutputMode::NativeDsd.
    case WasapiOutputMode::NativeDsd: {
        const auto rate = dsd_source_->format().sample_rate;
        source_channels = dsd_source_->format().channels;
        // Alt-setting 4 carries 32 raw DSD bits per frame per channel.
        if (rate % 32 != 0) {
            if (error_message) {
                start_failure_ = EngineStartFailure::FormatNegotiation;
                *error_message = "DSD rate does not divide into 32-bit Native DSD words";
            }
            return false;
        }
        output_sample_rate_ = static_cast<double>(rate / 32);
        source_frames_per_output_ = 4;
        break;
    }
    }
    if (source_channels == 0) {
        if (error_message) *error_message = "The source reports no channels";
        return false;
    }
    request.sample_rate = output_sample_rate_;
    request.channels = static_cast<std::uint16_t>(source_channels < 2 ? 2 : source_channels);

    WasapiNegotiatedFormat format;
    if (!session_.negotiate_format(request, &format, error_message)) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        return false;
    }
    if (mode_ != WasapiOutputMode::Pcm && format.container_bits < 24) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        if (error_message) *error_message = "The negotiated container cannot carry DSD payloads";
        return false;
    }
    if (mode_ == WasapiOutputMode::NativeDsd && format.container_bits != 32) {
        if (error_message) {
            start_failure_ = EngineStartFailure::FormatNegotiation;
            *error_message = "Native DSD requires the 32-bit (4-byte subslot) container";
        }
        return false;
    }
    if (!session_.initialize_exclusive(format, error_message)) return false;

    output_channels_ = format.channels;
    output_bytes_per_channel_ = format.bytes_per_sample();
    output_frame_bytes_ = format.bytes_per_frame();
    valid_bits_ = format.valid_bits;
    buffer_frame_count_ = session_.buffer_frame_count();
    format_description_ = format.description;
    if (output_frame_bytes_ == 0 || buffer_frame_count_ == 0) {
        if (error_message) *error_message = "The WASAPI stream reported an empty buffer";
        return false;
    }
    if (source_channels > output_channels_) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        if (error_message) *error_message = "The source has more channels than the endpoint";
        return false;
    }
    return true;
}

std::size_t WasapiPlayback::convert_pcm(std::size_t frames)
{
    const auto& source_format = pcm_source_->format();
    const auto source_channels = source_format.channels;
    const auto source_bytes_per_sample = source_format.bytes_per_sample();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < output_channels_; ++channel) {
            auto* target = conversion_scratch_.data() + frame * output_frame_bytes_ +
                           channel * output_bytes_per_channel_;
            if (channel < source_channels) {
                const auto* source = source_scratch_.data() +
                                     (frame * source_channels + channel) * source_bytes_per_sample;
                write_container_sample(target, output_bytes_per_channel_, valid_bits_,
                                       read_source_sample(source, source_format.encoding));
            } else {
                std::memset(target, 0, output_bytes_per_channel_);
            }
        }
    }
    return frames;
}

std::size_t WasapiPlayback::convert_dsd(std::size_t output_frames)
{
    const auto source_channels = dsd_source_->format().channels;
    for (std::size_t frame = 0; frame < output_frames; ++frame) {
        for (std::size_t channel = 0; channel < output_channels_; ++channel) {
            auto* target = conversion_scratch_.data() + frame * output_frame_bytes_ +
                           channel * output_bytes_per_channel_;
            // 0x55 is the canonical LSB1 silence pattern: DoP packs it into a
            // silent word, and native_word32 reverses it into the 0xaa the
            // Native DSD wire format uses. Channels the source does not provide
            // simply keep it.
            std::uint8_t raw[4] = {kDopSilenceByte, kDopSilenceByte, kDopSilenceByte,
                                   kDopSilenceByte};
            if (channel < source_channels) {
                for (std::size_t byte = 0; byte < source_frames_per_output_; ++byte) {
                    const auto index =
                        (frame * source_frames_per_output_ + byte) * source_channels + channel;
                    raw[byte] = source_scratch_[index];
                }
            }
            if (mode_ == WasapiOutputMode::NativeDsd) {
                DsdPacker::native_word32(raw, target);
            } else if (output_bytes_per_channel_ == 3) {
                DsdPacker::dop_int24(raw, kDopMarkerEven, target);
            } else {
                DsdPacker::dop_int32(raw, kDopMarkerEven, target);
            }
        }
    }
    return output_frames;
}

bool WasapiPlayback::request_seek(std::uint64_t source_frame)
{
    // Control thread only, so testing the state before publishing is enough:
    // neither the render thread nor the worker ever moves it into Requested.
    if (!running_.load(std::memory_order_acquire)) return false;
    if (seek_state_.load(std::memory_order_acquire) != SeekState::None) return false;
    if (source_frames_per_output_ == 0) return false;
    if (source_frame / source_frames_per_output_ > total_output_frames_) return false;
    seek_target_.store(source_frame, std::memory_order_release);
    seek_state_.store(SeekState::Requested, std::memory_order_release);
    return true;
}

bool WasapiPlayback::seek_source(std::uint64_t frame)
{
    return mode_ == WasapiOutputMode::Pcm ? pcm_source_->seek_frames(frame)
                                          : dsd_source_->seek_frames(frame);
}

bool WasapiPlayback::worker_handle_seek()
{
    switch (seek_state_.load(std::memory_order_acquire)) {
    case SeekState::Requested: {
        // Land on an output-frame boundary so DoP/native packing stays aligned
        // with the source bytes it consumes (a no-op when PCM maps 1:1).
        const std::uint64_t requested = seek_target_.load(std::memory_order_acquire);
        const std::uint64_t target =
            requested / source_frames_per_output_ * source_frames_per_output_;
        const bool pcm = mode_ == WasapiOutputMode::Pcm;
        if (seek_source(target)) {
            pending_output_position_.store(target / source_frames_per_output_,
                                           std::memory_order_release);
        } else {
            const std::uint64_t actual = pcm ? pcm_source_->position_frames()
                                             : dsd_source_->position_frames();
            pending_output_position_.store(actual / source_frames_per_output_,
                                           std::memory_order_release);
        }
        const bool finished = pcm ? pcm_source_->finished() : dsd_source_->finished();
        source_finished_.store(finished, std::memory_order_release);
        seek_state_.store(SeekState::SourceReady, std::memory_order_release);
        return true;
    }
    case SeekState::SourceReady:
        // Parked on purpose: the render thread clears the ring in this state
        // and must not race a producer.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    case SeekState::None:
        break;
    }
    return false;
}

bool WasapiPlayback::handle_transport(std::uint8_t* buffer, std::size_t bytes)
{
    const SeekState state = seek_state_.load(std::memory_order_acquire);
    if (state == SeekState::SourceReady) {
        // The worker is parked, so this consumer-side clear cannot race it.
        ring_->clear();
        output_position_frames_.store(pending_output_position_.load(std::memory_order_acquire),
                                      std::memory_order_relaxed);
        playback_finished_.store(false, std::memory_order_release);
        seek_state_.store(SeekState::None, std::memory_order_release);
        fill_output_silence(buffer, 0, bytes);
        return true;
    }
    if (state != SeekState::None || paused_.load(std::memory_order_acquire)) {
        // Mode-correct silence, and fill_render_buffer still stamps the DoP
        // marker afterwards, so the DAC keeps its DSD lock while paused.
        fill_output_silence(buffer, 0, bytes);
        return true;
    }
    return false;
}

void WasapiPlayback::run_source_worker()
{
    while (!stop_worker_.load(std::memory_order_acquire)) {
        if (worker_handle_seek()) continue;
        const std::size_t writable = ring_->available_to_write() / output_frame_bytes_;
        if (writable == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const std::size_t frames = std::min(worker_frames_, writable);
        std::size_t produced = 0;
        if (mode_ == WasapiOutputMode::Pcm) {
            produced = pcm_source_->read_frames(source_scratch_.data(), frames);
            if (produced == 0) {
                source_finished_.store(true, std::memory_order_release);
                // Stay alive instead of exiting: a seek backwards from the end
                // has to be able to restart production. stop() ends the thread.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            produced = convert_pcm(produced);
        } else {
            const std::size_t requested = frames * source_frames_per_output_;
            const std::size_t produced_source =
                dsd_source_->read_frames(source_scratch_.data(), requested);
            produced = produced_source / source_frames_per_output_;
            if (produced == 0) {
                source_finished_.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            produced = convert_dsd(produced);
        }
        const std::size_t bytes = produced * output_frame_bytes_;
        std::size_t written = 0;
        while (written < bytes && !stop_worker_.load(std::memory_order_acquire)) {
            written += ring_->write(conversion_scratch_.data() + written, bytes - written);
            if (written < bytes) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool finished = mode_ == WasapiOutputMode::Pcm ? pcm_source_->finished()
                                                             : dsd_source_->finished();
        if (finished) source_finished_.store(true, std::memory_order_release);
    }
}

void WasapiPlayback::fill_output_silence(std::uint8_t* buffer, std::size_t offset,
                                         std::size_t bytes)
{
    if (mode_ == WasapiOutputMode::Pcm) {
        std::memset(buffer + offset, 0, bytes);
        return;
    }
    if (mode_ == WasapiOutputMode::NativeDsd) {
        std::memset(buffer + offset, kNativeSilenceByte, bytes);
        return;
    }
    const std::uint8_t silence[] = {kDopSilenceByte, kDopSilenceByte};
    std::uint8_t word[4] = {};
    if (output_bytes_per_channel_ == 3) DsdPacker::dop_int24(silence, kDopMarkerEven, word);
    else DsdPacker::dop_int32(silence, kDopMarkerEven, word);
    // The ring can stop mid-sample, so honour the sample phase of the buffer.
    std::size_t position = offset;
    const std::size_t end = offset + bytes;
    while (position < end) {
        const std::size_t sample_offset = position % output_bytes_per_channel_;
        const std::size_t copy =
            std::min(output_bytes_per_channel_ - sample_offset, end - position);
        std::memcpy(buffer + position, word + sample_offset, copy);
        position += copy;
    }
}

void WasapiPlayback::fill_render_buffer(std::uint8_t* buffer, std::uint32_t frames)
{
    render_callbacks_.fetch_add(1, std::memory_order_relaxed);
    const std::size_t bytes = static_cast<std::size_t>(frames) * output_frame_bytes_;
    if (!handle_transport(buffer, bytes)) {
        const std::size_t received = ring_->read(buffer, bytes);
        if (source_finished_.load(std::memory_order_acquire) && ring_->available_to_read() == 0) {
            playback_finished_.store(true, std::memory_order_release);
        }
        if (received < bytes) {
            fill_output_silence(buffer, received, bytes - received);
            // A finite source drains to silence at the end; only a short read while
            // the worker still has data to make is a real underrun.
            if (!source_finished_.load(std::memory_order_acquire)) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Silence-filled frames carry no source data, so they do not move the
        // position; the counter stays aligned with the source.
        output_position_frames_.store(
            output_position_frames_.load(std::memory_order_relaxed) + received / output_frame_bytes_,
            std::memory_order_relaxed);
    }
    if (mode_ == WasapiOutputMode::DoP) {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const std::uint8_t marker =
                ((render_output_frames_ + frame) & 1u) == 0 ? kDopMarkerEven : kDopMarkerOdd;
            for (std::size_t channel = 0; channel < output_channels_; ++channel) {
                const std::size_t marker_offset =
                    (static_cast<std::size_t>(frame) * output_channels_ + channel) *
                        output_bytes_per_channel_ +
                    output_bytes_per_channel_ - 1;
                buffer[marker_offset] = marker;
            }
        }
        render_output_frames_ += frames;
    }
}

void WasapiPlayback::run_render_thread()
{
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    IAudioRenderClient* render = session_.render_client();
    const std::uint32_t frames = session_.buffer_frame_count();
    std::string failure;

    if (render == nullptr || frames == 0) {
        failure = "The WASAPI render client is not available";
    } else {
        // Pre-roll one full period so the device never starts on stale memory.
        BYTE* data = nullptr;
        const HRESULT hr = render->GetBuffer(frames, &data);
        if (FAILED(hr) || data == nullptr) {
            failure = std::string("IAudioRenderClient::GetBuffer failed: ") +
                      wasapi_error_name(hr);
        } else {
            fill_render_buffer(reinterpret_cast<std::uint8_t*>(data), frames);
            render->ReleaseBuffer(frames, 0);
        }
    }
    if (failure.empty() && !session_.start(&failure)) {
        if (failure.empty()) failure = "IAudioClient::Start failed";
    }
    if (!failure.empty()) {
        render_error_ = failure;
        render_state_.store(2, std::memory_order_release);
        if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
        if (SUCCEEDED(com_result)) CoUninitialize();
        return;
    }
    render_state_.store(1, std::memory_order_release);

    while (!stop_render_.load(std::memory_order_acquire)) {
        const DWORD wait = WaitForSingleObject(session_.event_handle(), 2000);
        if (stop_render_.load(std::memory_order_acquire)) break;
        if (wait != WAIT_OBJECT_0) {
            render_error_ = wait == WAIT_TIMEOUT
                                ? "The WASAPI render event timed out after 2000 ms"
                                : "Waiting on the WASAPI render event failed";
            break;
        }
        BYTE* data = nullptr;
        const HRESULT hr = render->GetBuffer(frames, &data);
        if (FAILED(hr) || data == nullptr) {
            render_error_ =
                std::string("IAudioRenderClient::GetBuffer failed: ") + wasapi_error_name(hr);
            break;
        }
        fill_render_buffer(reinterpret_cast<std::uint8_t*>(data), frames);
        const HRESULT release = render->ReleaseBuffer(frames, 0);
        if (FAILED(release)) {
            render_error_ = std::string("IAudioRenderClient::ReleaseBuffer failed: ") +
                            wasapi_error_name(release);
            break;
        }
    }

    if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(com_result)) CoUninitialize();
}

} // namespace wasio
