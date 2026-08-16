#include "playback/pcm_playback.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace wasio {
namespace {

std::atomic<PcmPlayback*> active_playback{nullptr};

bool is_success(ASIOError error)
{
    return error == ASE_OK || error == ASE_SUCCESS;
}

bool map_asio_type(ASIOSampleType type, PcmEncoding* encoding)
{
    switch (type) {
    case ASIOSTInt16LSB: *encoding = PcmEncoding::Int16; return true;
    case ASIOSTInt24LSB: *encoding = PcmEncoding::Int24; return true;
    case ASIOSTInt32LSB: *encoding = PcmEncoding::Int32; return true;
    default: return false;
    }
}

std::int32_t read_source_sample(const std::uint8_t* source, PcmEncoding encoding)
{
    switch (encoding) {
    case PcmEncoding::Int16: {
        const auto value = static_cast<std::int16_t>(source[0] | (source[1] << 8));
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(
            static_cast<std::int32_t>(value)) << 16);
    }
    case PcmEncoding::Int24: {
        std::int32_t value = source[0] | (source[1] << 8) | (source[2] << 16);
        if ((value & 0x00800000) != 0) value |= static_cast<std::int32_t>(0xff000000);
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << 8);
    }
    case PcmEncoding::Int32:
        return static_cast<std::int32_t>(source[0] | (source[1] << 8) |
                                          (source[2] << 16) | (source[3] << 24));
    }
    return 0;
}

void write_target_sample(std::uint8_t* destination, PcmEncoding encoding, std::int32_t value)
{
    switch (encoding) {
    case PcmEncoding::Int16: {
        const auto sample = static_cast<std::int16_t>(value >> 16);
        destination[0] = static_cast<std::uint8_t>(sample);
        destination[1] = static_cast<std::uint8_t>(sample >> 8);
        break;
    }
    case PcmEncoding::Int24:
        destination[0] = static_cast<std::uint8_t>(value >> 8);
        destination[1] = static_cast<std::uint8_t>(value >> 16);
        destination[2] = static_cast<std::uint8_t>(value >> 24);
        break;
    case PcmEncoding::Int32:
        destination[0] = static_cast<std::uint8_t>(value);
        destination[1] = static_cast<std::uint8_t>(value >> 8);
        destination[2] = static_cast<std::uint8_t>(value >> 16);
        destination[3] = static_cast<std::uint8_t>(value >> 24);
        break;
    }
}

} // namespace

PcmPlayback::~PcmPlayback()
{
    stop();
}

bool PcmPlayback::start(PcmPlaybackConfig config, std::string* error_message)
{
    stop();
    // Anything that fails before configure_output() classifies the format is a
    // device problem; the format sites below override this.
    start_failure_ = EngineStartFailure::DeviceOpen;
    if (!config.source || !config.source->format().valid()) {
        if (error_message) *error_message = "PCM source is missing or invalid";
        return false;
    }
    if (config.source->format().channels == 0 || config.ring_frames < 2) {
        if (error_message) *error_message = "PCM source/ring configuration is invalid";
        return false;
    }
    source_ = std::move(config.source);
    if (!session_.open(config.driver_name, error_message)) {
        source_.reset();
        return false;
    }
    if (!configure_output(error_message)) {
        stop();
        return false;
    }

    output_format_.sample_rate = source_->format().sample_rate;
    output_format_.channels = static_cast<std::size_t>(output_channels_);
    const std::size_t source_frame_bytes = source_->format().bytes_per_frame();
    const std::size_t output_frame_bytes = output_format_.bytes_per_frame();
    ring_ = std::make_unique<SpscByteRing>(config.ring_frames * output_frame_bytes);
    source_scratch_.resize(1024 * source_frame_bytes);
    conversion_scratch_.resize(1024 * output_frame_bytes);
    callback_scratch_.resize(static_cast<std::size_t>(buffer_size_) * output_frame_bytes);

    stop_worker_.store(false, std::memory_order_release);
    source_finished_.store(false, std::memory_order_release);
    playback_finished_.store(false, std::memory_order_release);
    underruns_.store(0, std::memory_order_relaxed);
    buffer_switches_.store(0, std::memory_order_relaxed);
    paused_.store(false, std::memory_order_release);
    seek_state_.store(SeekState::None, std::memory_order_release);
    seek_target_.store(0, std::memory_order_relaxed);
    pending_output_position_.store(0, std::memory_order_relaxed);
    output_position_frames_.store(0, std::memory_order_relaxed);
    total_output_frames_ = source_->total_frames();
    source_worker_ = std::thread(&PcmPlayback::run_source_worker, this);

    // Prime at least two ASIO periods before handing control to the driver. The
    // wait occurs on the start/control thread, never from the callback.
    const std::size_t prime_bytes = static_cast<std::size_t>(buffer_size_) *
                                    output_format_.bytes_per_frame() * 2;
    for (int attempt = 0; attempt < 500 && ring_->available_to_read() < prime_bytes &&
                              !source_finished_.load(std::memory_order_acquire); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    active_playback.store(this, std::memory_order_release);
    const ASIOError start_error = session_.start();
    if (!is_success(start_error)) {
        active_playback.store(nullptr, std::memory_order_release);
        if (error_message) *error_message = std::string("ASIOStart failed: ") +
                                             asio_error_name(start_error);
        stop();
        return false;
    }
    start_failure_ = EngineStartFailure::None;
    running_.store(true, std::memory_order_release);
    return true;
}

void PcmPlayback::stop()
{
    const bool was_active = active_playback.load(std::memory_order_acquire) == this;
    if (was_active && session_.is_open()) session_.stop();
    if (was_active) active_playback.store(nullptr, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    playback_finished_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    seek_state_.store(SeekState::None, std::memory_order_release);
    stop_worker_.store(true, std::memory_order_release);
    if (source_worker_.joinable()) source_worker_.join();
    if (session_.is_open()) session_.dispose_buffers();
    buffers_.clear();
    session_.close();
    ring_.reset();
    source_.reset();
    source_scratch_.clear();
    conversion_scratch_.clear();
    callback_scratch_.clear();
}

bool PcmPlayback::configure_output(std::string* error_message)
{
    long inputs = 0;
    long outputs = 0;
    if (!is_success(session_.get_channels(&inputs, &outputs)) || outputs <= 0) {
        if (error_message) *error_message = "ASIO driver has no output channels";
        return false;
    }
    output_channels_ = outputs;

    long min_size = 0;
    long max_size = 0;
    long preferred = 0;
    long granularity = 0;
    if (!is_success(session_.get_buffer_size(&min_size, &max_size, &preferred, &granularity))) {
        if (error_message) *error_message = "ASIOGetBufferSize failed";
        return false;
    }
    buffer_size_ = preferred;
    if (buffer_size_ <= 0) buffer_size_ = min_size;
    if (buffer_size_ <= 0) {
        if (error_message) *error_message = "ASIO driver returned an invalid buffer size";
        return false;
    }

    const auto& source_format = source_->format();
    if (source_format.channels > static_cast<std::size_t>(output_channels_)) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        if (error_message) *error_message = "PCM source has more channels than ASIO output";
        return false;
    }
    ASIOChannelInfo channel = {};
    channel.channel = 0;
    channel.isInput = ASIOFalse;
    const ASIOError channel_error = session_.get_channel_info(&channel);
    if (!is_success(channel_error) || !map_asio_type(channel.type, &output_format_.encoding)) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        if (error_message) *error_message = "ASIO output sample type is not supported";
        return false;
    }
    target_bytes_per_sample_ = output_format_.bytes_per_sample();

    const ASIOError rate_error = session_.set_sample_rate(source_format.sample_rate);
    if (!is_success(rate_error)) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        if (error_message) *error_message = std::string("ASIOSetSampleRate failed: ") +
                                             asio_error_name(rate_error);
        return false;
    }

    buffers_.resize(static_cast<std::size_t>(output_channels_));
    for (long channel_index = 0; channel_index < output_channels_; ++channel_index) {
        buffers_[static_cast<std::size_t>(channel_index)] = {};
        buffers_[static_cast<std::size_t>(channel_index)].isInput = ASIOFalse;
        buffers_[static_cast<std::size_t>(channel_index)].channelNum = channel_index;
    }
    callbacks_.bufferSwitch = &PcmPlayback::buffer_switch;
    callbacks_.sampleRateDidChange = &PcmPlayback::sample_rate_did_change;
    callbacks_.asioMessage = &PcmPlayback::asio_message;
    callbacks_.bufferSwitchTimeInfo = &PcmPlayback::buffer_switch_time_info;
    const ASIOError create_error = session_.create_buffers(buffers_.data(), output_channels_,
                                                            buffer_size_, &callbacks_);
    if (!is_success(create_error)) {
        if (error_message) *error_message = std::string("ASIOCreateBuffers failed: ") +
                                             asio_error_name(create_error);
        buffers_.clear();
        return false;
    }
    return true;
}

void PcmPlayback::run_source_worker()
{
    const auto source_frame_bytes = source_->format().bytes_per_frame();
    const auto output_frame_bytes = output_format_.bytes_per_frame();
    const auto source_channels = source_->format().channels;
    const auto output_channels = static_cast<std::size_t>(output_channels_);
    const auto source_bytes_per_sample = source_->format().bytes_per_sample();
    const auto worker_frames = source_scratch_.size() / source_frame_bytes;

    while (!stop_worker_.load(std::memory_order_acquire)) {
        if (worker_handle_seek()) continue;
        const std::size_t writable_frames = ring_->available_to_write() / output_frame_bytes;
        if (writable_frames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const std::size_t frames = std::min(worker_frames, writable_frames);
        const std::size_t produced = source_->read_frames(source_scratch_.data(), frames);
        if (produced == 0) {
            source_finished_.store(true, std::memory_order_release);
            // Stay alive instead of exiting: a seek backwards from the end has
            // to be able to restart production. stop() ends the thread.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        for (std::size_t frame = 0; frame < produced; ++frame) {
            for (std::size_t channel = 0; channel < output_channels; ++channel) {
                auto* target = conversion_scratch_.data() + (frame * output_channels + channel) *
                               target_bytes_per_sample_;
                if (channel < source_channels) {
                    const auto* source = source_scratch_.data() +
                        (frame * source_channels + channel) * source_bytes_per_sample;
                    write_target_sample(target, output_format_.encoding,
                                        read_source_sample(source, source_->format().encoding));
                } else {
                    std::memset(target, 0, target_bytes_per_sample_);
                }
            }
        }
        const std::size_t bytes = produced * output_frame_bytes;
        std::size_t written = 0;
        while (written < bytes && !stop_worker_.load(std::memory_order_acquire)) {
            written += ring_->write(conversion_scratch_.data() + written, bytes - written);
            if (written < bytes) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (source_->finished()) source_finished_.store(true, std::memory_order_release);
    }
}

bool PcmPlayback::request_seek(std::uint64_t source_frame)
{
    // Control thread only, so testing the state before publishing is enough:
    // neither the callback nor the worker ever moves it into Requested.
    if (!running_.load(std::memory_order_acquire)) return false;
    if (seek_state_.load(std::memory_order_acquire) != SeekState::None) return false;
    if (source_frame > total_output_frames_) return false;
    seek_target_.store(source_frame, std::memory_order_release);
    seek_state_.store(SeekState::Requested, std::memory_order_release);
    return true;
}

bool PcmPlayback::worker_handle_seek()
{
    switch (seek_state_.load(std::memory_order_acquire)) {
    case SeekState::Requested: {
        const std::uint64_t target = seek_target_.load(std::memory_order_acquire);
        if (source_->seek_frames(target)) {
            pending_output_position_.store(target, std::memory_order_release);
        } else {
            // A rejected seek leaves the source where it was, so report that
            // position instead of the one that was asked for.
            pending_output_position_.store(source_->position_frames(), std::memory_order_release);
        }
        // Seeking backwards out of the finished state has to resume production.
        source_finished_.store(source_->finished(), std::memory_order_release);
        seek_state_.store(SeekState::SourceReady, std::memory_order_release);
        return true;
    }
    case SeekState::SourceReady:
        // Parked on purpose: the callback clears the ring in this state and
        // must not race a producer, so nothing may be written until it is done.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    case SeekState::None:
        break;
    }
    return false;
}

bool PcmPlayback::handle_transport(std::size_t requested_bytes)
{
    const SeekState state = seek_state_.load(std::memory_order_acquire);
    if (state == SeekState::SourceReady) {
        // The worker is parked, so this consumer-side clear cannot race it and
        // no pre-seek frames can be appended afterwards.
        ring_->clear();
        output_position_frames_.store(pending_output_position_.load(std::memory_order_acquire),
                                      std::memory_order_relaxed);
        playback_finished_.store(false, std::memory_order_release);
        seek_state_.store(SeekState::None, std::memory_order_release);
        std::memset(callback_scratch_.data(), 0, requested_bytes);
        return true;
    }
    if (state != SeekState::None || paused_.load(std::memory_order_acquire)) {
        std::memset(callback_scratch_.data(), 0, requested_bytes);
        return true;
    }
    return false;
}

void PcmPlayback::fill_silence()
{
    for (auto& buffer : buffers_) {
        if (buffer.buffers[0] != nullptr) {
            std::memset(buffer.buffers[0], 0,
                        static_cast<std::size_t>(buffer_size_) * target_bytes_per_sample_);
        }
        if (buffer.buffers[1] != nullptr) {
            std::memset(buffer.buffers[1], 0,
                        static_cast<std::size_t>(buffer_size_) * target_bytes_per_sample_);
        }
    }
}

void PcmPlayback::on_buffer_switch(long double_buffer_index)
{
    buffer_switches_.fetch_add(1, std::memory_order_relaxed);
    const std::size_t output_frame_bytes = output_format_.bytes_per_frame();
    const std::size_t requested = static_cast<std::size_t>(buffer_size_) * output_frame_bytes;
    if (!handle_transport(requested)) {
        const std::size_t received = ring_->read(callback_scratch_.data(), requested);
        if (source_finished_.load(std::memory_order_acquire) &&
            ring_->available_to_read() == 0) {
            playback_finished_.store(true, std::memory_order_release);
        }
        if (received < requested) {
            std::memset(callback_scratch_.data() + received, 0, requested - received);
            // A finite source naturally drains to silence at the end; only count
            // a short buffer as an underrun while the worker still has data to make.
            if (!source_finished_.load(std::memory_order_acquire)) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Silence-filled frames carry no source data, so they do not move the
        // position; the counter stays aligned with the source.
        output_position_frames_.store(
            output_position_frames_.load(std::memory_order_relaxed) + received / output_frame_bytes,
            std::memory_order_relaxed);
    }
    for (std::size_t channel = 0; channel < buffers_.size(); ++channel) {
        auto* destination = static_cast<std::uint8_t*>(
            buffers_[channel].buffers[double_buffer_index]);
        for (long frame = 0; frame < buffer_size_; ++frame) {
            const auto* source = callback_scratch_.data() +
                (static_cast<std::size_t>(frame) * buffers_.size() + channel) *
                target_bytes_per_sample_;
            std::memcpy(destination + static_cast<std::size_t>(frame) * target_bytes_per_sample_,
                        source, target_bytes_per_sample_);
        }
    }
    session_.output_ready();
}

void PcmPlayback::buffer_switch(long double_buffer_index, ASIOBool)
{
    if (auto* playback = active_playback.load(std::memory_order_acquire)) {
        playback->on_buffer_switch(double_buffer_index);
    }
}

void PcmPlayback::sample_rate_did_change(ASIOSampleRate)
{
}

long PcmPlayback::asio_message(long selector, long, void*, double*)
{
    if (selector == kAsioSelectorSupported) return 0;
    if (selector == kAsioEngineVersion) return 2;
    return 0;
}

ASIOTime* PcmPlayback::buffer_switch_time_info(ASIOTime* params, long double_buffer_index,
                                                ASIOBool)
{
    if (auto* playback = active_playback.load(std::memory_order_acquire)) {
        playback->on_buffer_switch(double_buffer_index);
    }
    return params;
}

} // namespace wasio
