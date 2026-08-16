#include "playback/dsd_playback.h"
#include "audio/dsd_packer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace wasio {
namespace {

std::atomic<DsdPlayback*> active_dsd_playback{nullptr};

bool is_success(ASIOError error)
{
    return error == ASE_OK || error == ASE_SUCCESS;
}

} // namespace

DsdPlayback::~DsdPlayback()
{
    stop();
}

bool DsdPlayback::start(DsdPlaybackConfig config, std::string* error_message)
{
    stop();
    start_failure_ = EngineStartFailure::DeviceOpen;
    if (!config.source || !config.source->format().valid() || config.ring_frames < 2) {
        if (error_message) *error_message = "DSD source or ring configuration is invalid";
        return false;
    }
    if (config.mode == DsdOutputMode::DoP && config.source->format().sample_rate > kMaxDopDsdRate) {
        if (error_message) *error_message =
            "DoP512 is not supported: it requires a 1,411,200 Hz PCM host rate";
        return false;
    }
    source_ = std::move(config.source);
    dsd_sample_rate_ = source_->format().sample_rate;
    mode_ = config.mode;
    if (!session_.open(config.driver_name, error_message)) {
        source_.reset();
        return false;
    }
    if (!configure_output(error_message)) {
        stop();
        return false;
    }

    const std::size_t output_frame_bytes = output_channels_ * output_bytes_per_channel_;
    const std::size_t ring_frames = std::max<std::size_t>(
        config.ring_frames, static_cast<std::size_t>(callback_frames_) * 4);
    ring_ = std::make_unique<SpscByteRing>(ring_frames * output_frame_bytes);
    const std::size_t worker_frames = 512;
    source_scratch_.resize(worker_frames * source_->format().bytes_per_frame() *
                            source_frames_per_output_);
    conversion_scratch_.resize(worker_frames * output_frame_bytes);
    callback_scratch_.resize(static_cast<std::size_t>(callback_frames_) * output_frame_bytes);

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
    total_output_frames_ = source_->total_frames() / source_frames_per_output_;
    callback_output_frames_ = 0;
    source_worker_ = std::thread(&DsdPlayback::run_source_worker, this);

    const std::size_t prime_bytes = static_cast<std::size_t>(callback_frames_) * output_frame_bytes * 2;
    for (int attempt = 0; attempt < 500 && ring_->available_to_read() < prime_bytes &&
                              !source_finished_.load(std::memory_order_acquire); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    active_dsd_playback.store(this, std::memory_order_release);
    const ASIOError start_error = session_.start();
    if (!is_success(start_error)) {
        active_dsd_playback.store(nullptr, std::memory_order_release);
        if (error_message) *error_message = std::string("ASIOStart failed: ") +
                                             asio_error_name(start_error);
        stop();
        return false;
    }
    start_failure_ = EngineStartFailure::None;
    running_.store(true, std::memory_order_release);
    return true;
}

void DsdPlayback::stop()
{
    const bool active = active_dsd_playback.load(std::memory_order_acquire) == this;
    if (active) active_dsd_playback.store(nullptr, std::memory_order_release);
    if (active && session_.is_open()) session_.stop();
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

bool DsdPlayback::configure_output(std::string* error_message)
{
    ASIOIoFormat io_format = {};
    io_format.FormatType = mode_ == DsdOutputMode::Native ? kASIODSDFormat : kASIOPCMFormat;
    if (!is_success(session_.future(kAsioSetIoFormat, &io_format))) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        if (error_message) *error_message = "ASIO driver rejected the requested I/O format";
        return false;
    }
    const auto dsd_rate = source_->format().sample_rate;
    if (mode_ == DsdOutputMode::Native) {
        output_sample_rate_ = dsd_rate;
        source_frames_per_output_ = 4; // CT ASIO exposes a 32-bit container in Native DSD mode.
        output_bytes_per_channel_ = 4;
    } else {
        output_sample_rate_ = dsd_rate / 16;
        source_frames_per_output_ = 2;
        if (output_sample_rate_ == 0 || dsd_rate % 16 != 0) {
            start_failure_ = EngineStartFailure::FormatNegotiation;
            if (error_message) *error_message = "DSD rate cannot be represented by DoP";
            return false;
        }
    }
    if (!is_success(session_.set_sample_rate(static_cast<double>(output_sample_rate_)))) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        if (error_message) *error_message = "ASIOSetSampleRate rejected the DSD/DoP rate";
        return false;
    }

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
    buffer_size_ = preferred > 0 ? preferred : min_size;
    if (buffer_size_ <= 0) {
        if (error_message) *error_message = "ASIO driver returned an invalid DSD buffer size";
        return false;
    }
    callback_frames_ = mode_ == DsdOutputMode::Native ? buffer_size_ / 32 : buffer_size_;
    if (callback_frames_ <= 0 || (mode_ == DsdOutputMode::Native && buffer_size_ % 32 != 0)) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        if (error_message) *error_message = "Native DSD buffer size is not 32-bit aligned";
        return false;
    }

    ASIOChannelInfo channel = {};
    channel.channel = 0;
    channel.isInput = ASIOFalse;
    if (!is_success(session_.get_channel_info(&channel))) {
        if (error_message) *error_message = "ASIOGetChannelInfo failed";
        return false;
    }
    output_sample_type_ = channel.type;
    if (mode_ == DsdOutputMode::Native && channel.type != ASIOSTDSDInt8MSB1 &&
        channel.type != ASIOSTDSDInt8LSB1 && channel.type != ASIOSTDSDInt8NER8) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        if (error_message) *error_message = "ASIO output channel is not a Native DSD type";
        return false;
    }
    if (mode_ == DsdOutputMode::DoP && channel.type != ASIOSTInt32LSB &&
        channel.type != ASIOSTInt24LSB) {
        start_failure_ = EngineStartFailure::FormatNegotiation;
        if (error_message) *error_message = "ASIO output channel is not a 24-bit PCM container";
        return false;
    }
    if (mode_ == DsdOutputMode::DoP) {
        output_bytes_per_channel_ = channel.type == ASIOSTInt24LSB ? 3 : 4;
    }

    buffers_.resize(static_cast<std::size_t>(output_channels_));
    for (long index = 0; index < output_channels_; ++index) {
        buffers_[static_cast<std::size_t>(index)] = {};
        buffers_[static_cast<std::size_t>(index)].isInput = ASIOFalse;
        buffers_[static_cast<std::size_t>(index)].channelNum = index;
    }
    callbacks_.bufferSwitch = &DsdPlayback::buffer_switch;
    callbacks_.sampleRateDidChange = &DsdPlayback::sample_rate_did_change;
    callbacks_.asioMessage = &DsdPlayback::asio_message;
    callbacks_.bufferSwitchTimeInfo = &DsdPlayback::buffer_switch_time_info;
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

void DsdPlayback::run_source_worker()
{
    const auto source_channels = source_->format().channels;
    const auto output_channels = static_cast<std::size_t>(output_channels_);
    const auto source_frame_bytes = source_->format().bytes_per_frame();
    const auto output_frame_bytes = output_channels * output_bytes_per_channel_;
    const auto worker_frames = source_scratch_.size() /
                               (source_frame_bytes * source_frames_per_output_);
    while (!stop_worker_.load(std::memory_order_acquire)) {
        if (worker_handle_seek()) continue;
        const std::size_t writable = ring_->available_to_write() / output_frame_bytes;
        if (writable == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const std::size_t frames = std::min(worker_frames, writable);
        const std::size_t requested_source_frames = frames * source_frames_per_output_;
        const std::size_t produced_source_frames = source_->read_frames(
            source_scratch_.data(), requested_source_frames);
        const std::size_t produced_output_frames = produced_source_frames / source_frames_per_output_;
        if (produced_output_frames == 0) {
            source_finished_.store(true, std::memory_order_release);
            // Stay alive instead of exiting: a seek backwards from the end has
            // to be able to restart production. stop() ends the thread.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        for (std::size_t frame = 0; frame < produced_output_frames; ++frame) {
            for (std::size_t channel = 0; channel < output_channels; ++channel) {
                auto* target = conversion_scratch_.data() + frame * output_frame_bytes +
                               channel * output_bytes_per_channel_;
                if (channel >= source_channels) {
                    if (mode_ == DsdOutputMode::Native) {
                        std::memset(target, 0xaa, output_bytes_per_channel_);
                    } else {
                        const std::uint8_t silence[] = {0x55, 0x55};
                        if (output_sample_type_ == ASIOSTInt24LSB)
                            DsdPacker::dop_int24(silence, 0x05, target);
                        else
                            DsdPacker::dop_int32(silence, 0x05, target);
                    }
                    continue;
                }
                if (mode_ == DsdOutputMode::Native) {
                    for (std::size_t byte = 0; byte < source_frames_per_output_; ++byte) {
                        const auto source_index = (frame * source_frames_per_output_ + byte) *
                                                  source_channels + channel;
                        target[byte] = DsdPacker::reverse_bits(source_scratch_[source_index]);
                    }
                } else {
                    const std::uint8_t dsd_bytes[] = {
                        source_scratch_[(frame * source_frames_per_output_) *
                                        source_channels + channel],
                        source_scratch_[(frame * source_frames_per_output_ + 1) *
                                        source_channels + channel],
                    };
                    if (output_sample_type_ == ASIOSTInt24LSB)
                        DsdPacker::dop_int24(dsd_bytes, 0x05, target);
                    else
                        DsdPacker::dop_int32(dsd_bytes, 0x05, target);
                }
            }
        }
        const std::size_t bytes = produced_output_frames * output_frame_bytes;
        std::size_t written = 0;
        while (written < bytes && !stop_worker_.load(std::memory_order_acquire)) {
            written += ring_->write(conversion_scratch_.data() + written, bytes - written);
            if (written < bytes) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (source_->finished()) source_finished_.store(true, std::memory_order_release);
    }
}

bool DsdPlayback::request_seek(std::uint64_t source_frame)
{
    // Control thread only, so testing the state before publishing is enough:
    // neither the callback nor the worker ever moves it into Requested.
    if (!running_.load(std::memory_order_acquire)) return false;
    if (seek_state_.load(std::memory_order_acquire) != SeekState::None) return false;
    if (source_frames_per_output_ == 0) return false;
    if (source_frame / source_frames_per_output_ > total_output_frames_) return false;
    seek_target_.store(source_frame, std::memory_order_release);
    seek_state_.store(SeekState::Requested, std::memory_order_release);
    return true;
}

bool DsdPlayback::worker_handle_seek()
{
    switch (seek_state_.load(std::memory_order_acquire)) {
    case SeekState::Requested: {
        // Land on an output-word boundary so the DoP/native packing stays
        // aligned with the source bytes it consumes.
        const std::uint64_t requested = seek_target_.load(std::memory_order_acquire);
        const std::uint64_t target =
            requested / source_frames_per_output_ * source_frames_per_output_;
        if (source_->seek_frames(target)) {
            pending_output_position_.store(target / source_frames_per_output_,
                                           std::memory_order_release);
        } else {
            pending_output_position_.store(
                source_->position_frames() / source_frames_per_output_, std::memory_order_release);
        }
        source_finished_.store(source_->finished(), std::memory_order_release);
        seek_state_.store(SeekState::SourceReady, std::memory_order_release);
        return true;
    }
    case SeekState::SourceReady:
        // Parked on purpose: the callback clears the ring in this state and
        // must not race a producer.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    case SeekState::None:
        break;
    }
    return false;
}

bool DsdPlayback::handle_transport(std::size_t requested_bytes)
{
    const SeekState state = seek_state_.load(std::memory_order_acquire);
    if (state == SeekState::SourceReady) {
        // The worker is parked, so this consumer-side clear cannot race it.
        ring_->clear();
        output_position_frames_.store(pending_output_position_.load(std::memory_order_acquire),
                                      std::memory_order_relaxed);
        playback_finished_.store(false, std::memory_order_release);
        seek_state_.store(SeekState::None, std::memory_order_release);
        fill_dsd_silence(callback_scratch_.data(), requested_bytes);
        return true;
    }
    if (state != SeekState::None || paused_.load(std::memory_order_acquire)) {
        // DSD silence rather than zeros, and on_buffer_switch still stamps the
        // DoP marker afterwards, so the DAC keeps its DSD lock while paused.
        fill_dsd_silence(callback_scratch_.data(), requested_bytes);
        return true;
    }
    return false;
}

void DsdPlayback::fill_dsd_silence(std::uint8_t* destination, std::size_t bytes)
{
    if (mode_ == DsdOutputMode::Native) {
        std::memset(destination, 0xaa, bytes);
        return;
    }
    const std::size_t frame_bytes = static_cast<std::size_t>(output_channels_) *
                                    output_bytes_per_channel_;
    const std::uint8_t silence[] = {0x55, 0x55};
    std::size_t offset = 0;
    while (offset + frame_bytes <= bytes) {
        for (long channel = 0; channel < output_channels_; ++channel) {
            auto* target = destination + offset +
                           static_cast<std::size_t>(channel) * output_bytes_per_channel_;
            if (output_sample_type_ == ASIOSTInt24LSB)
                DsdPacker::dop_int24(silence, 0x05, target);
            else
                DsdPacker::dop_int32(silence, 0x05, target);
        }
        offset += frame_bytes;
    }
    if (offset < bytes) std::memset(destination + offset, 0, bytes - offset);
}

void DsdPlayback::on_buffer_switch(long double_buffer_index)
{
    buffer_switches_.fetch_add(1, std::memory_order_relaxed);
    const std::size_t bytes = static_cast<std::size_t>(callback_frames_) * output_channels_ *
                               output_bytes_per_channel_;
    if (!handle_transport(bytes)) {
        const std::size_t received = ring_->read(callback_scratch_.data(), bytes);
        if (source_finished_.load(std::memory_order_acquire) &&
            ring_->available_to_read() == 0) {
            playback_finished_.store(true, std::memory_order_release);
        }
        if (received < bytes) {
            fill_dsd_silence(callback_scratch_.data() + received, bytes - received);
            if (!source_finished_.load(std::memory_order_acquire)) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Silence-filled frames carry no source data, so they do not move the
        // position; the counter stays aligned with the source.
        const std::size_t output_frame_bytes =
            static_cast<std::size_t>(output_channels_) * output_bytes_per_channel_;
        output_position_frames_.store(
            output_position_frames_.load(std::memory_order_relaxed) + received / output_frame_bytes,
            std::memory_order_relaxed);
    }
    if (mode_ == DsdOutputMode::DoP) {
        // Marker phase belongs to the delivered ASIO timeline, not the worker
        // timeline. Stamp every frame here so an underrun cannot break the
        // required 0x05/0xfa alternation or race with the producer thread.
        for (long frame = 0; frame < callback_frames_; ++frame) {
            const std::uint8_t marker = ((callback_output_frames_ + frame) & 1u) == 0
                                            ? 0x05 : 0xfa;
            for (std::size_t channel = 0; channel < buffers_.size(); ++channel) {
                const auto marker_offset =
                    (static_cast<std::size_t>(frame) * buffers_.size() + channel) *
                        output_bytes_per_channel_ +
                    output_bytes_per_channel_ - 1;
                callback_scratch_[marker_offset] = marker;
            }
        }
        callback_output_frames_ += static_cast<std::uint64_t>(callback_frames_);
    }
    for (std::size_t channel = 0; channel < buffers_.size(); ++channel) {
        auto* destination = static_cast<std::uint8_t*>(buffers_[channel].buffers[double_buffer_index]);
        for (long frame = 0; frame < callback_frames_; ++frame) {
            const auto* source = callback_scratch_.data() +
                (static_cast<std::size_t>(frame) * buffers_.size() + channel) *
                output_bytes_per_channel_;
            std::memcpy(destination + static_cast<std::size_t>(frame) * output_bytes_per_channel_,
                        source, output_bytes_per_channel_);
        }
    }
    session_.output_ready();
}

void DsdPlayback::buffer_switch(long double_buffer_index, ASIOBool)
{
    if (auto* playback = active_dsd_playback.load(std::memory_order_acquire)) {
        playback->on_buffer_switch(double_buffer_index);
    }
}

void DsdPlayback::sample_rate_did_change(ASIOSampleRate)
{
}

long DsdPlayback::asio_message(long selector, long, void*, double*)
{
    if (selector == kAsioEngineVersion) return 2;
    return 0;
}

ASIOTime* DsdPlayback::buffer_switch_time_info(ASIOTime* params, long double_buffer_index,
                                                ASIOBool)
{
    if (auto* playback = active_dsd_playback.load(std::memory_order_acquire)) {
        playback->on_buffer_switch(double_buffer_index);
    }
    return params;
}

} // namespace wasio
