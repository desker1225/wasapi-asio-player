#pragma once

#include "asio/asio_session.h"
#include "playback/ring_buffer.h"
#include "playback/playback_engine.h"
#include "playback/seek_state.h"
#include "sources/audio_source.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace wasio {

struct PcmPlaybackConfig {
    std::string driver_name;
    std::unique_ptr<AudioSource> source;
    std::size_t ring_frames = 16384;
    long buffer_size = 0; // zero selects the driver's preferred size
};

class PcmPlayback final {
public:
    PcmPlayback() = default;
    ~PcmPlayback();

    PcmPlayback(const PcmPlayback&) = delete;
    PcmPlayback& operator=(const PcmPlayback&) = delete;

    bool start(PcmPlaybackConfig config, std::string* error_message);
    // Why the last start() failed, so callers can separate "device will not
    // open" from "device will not take this format".
    EngineStartFailure start_failure() const { return start_failure_; }
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    bool source_finished() const { return source_finished_.load(std::memory_order_acquire); }
    bool playback_finished() const { return playback_finished_.load(std::memory_order_acquire); }
    std::uint64_t underrun_count() const { return underruns_.load(std::memory_order_relaxed); }
    std::uint64_t buffer_switch_count() const { return buffer_switches_.load(std::memory_order_relaxed); }
    const AudioFormat& output_format() const { return output_format_; }
    long buffer_size() const { return buffer_size_; }

    // ---- transport ----
    // The device stays open while paused: the callback outputs silence without
    // consuming the ring, counting an underrun, or advancing the position.
    void set_paused(bool paused) { paused_.store(paused, std::memory_order_release); }
    bool is_paused() const { return paused_.load(std::memory_order_acquire); }
    // Output frames actually delivered from the source, so
    // position_seconds = output_position_frames() / output_format().sample_rate.
    std::uint64_t output_position_frames() const
    {
        return output_position_frames_.load(std::memory_order_relaxed);
    }
    std::uint64_t total_output_frames() const { return total_output_frames_; }
    // Frames per second for output_position_frames(); for PCM the output frame
    // rate is simply the file's sample rate.
    double output_frame_rate() const { return output_format_.sample_rate; }
    // PCM plays one source frame per output frame.
    std::size_t source_frames_per_output() const { return 1; }
    // Call from the control thread only. Returns false if a seek is already in
    // flight or playback is not running.
    bool request_seek(std::uint64_t source_frame);
    bool seek_in_progress() const
    {
        return seek_state_.load(std::memory_order_acquire) != SeekState::None;
    }

private:
    static void buffer_switch(long double_buffer_index, ASIOBool direct_process);
    static void sample_rate_did_change(ASIOSampleRate rate);
    static long asio_message(long selector, long value, void* message, double* opt);
    static ASIOTime* buffer_switch_time_info(ASIOTime* params, long double_buffer_index,
                                             ASIOBool direct_process);

    void on_buffer_switch(long double_buffer_index);
    void run_source_worker();
    bool configure_output(std::string* error_message);
    void fill_silence();
    // Realtime side. Fills callback_scratch_ with silence and returns true when
    // pause or a pending seek owns this period.
    bool handle_transport(std::size_t requested_bytes);
    // Worker side. Returns true when it handled a seek or is parked for one.
    bool worker_handle_seek();

    AsioSession session_;
    std::unique_ptr<AudioSource> source_;
    std::unique_ptr<SpscByteRing> ring_;
    AudioFormat output_format_;
    long buffer_size_ = 0;
    long output_channels_ = 0;
    std::size_t target_bytes_per_sample_ = 0;
    std::vector<ASIOBufferInfo> buffers_;
    ASIOCallbacks callbacks_ = {};
    std::vector<std::uint8_t> callback_scratch_;
    std::vector<std::uint8_t> source_scratch_;
    std::vector<std::uint8_t> conversion_scratch_;
    std::atomic<bool> stop_worker_{false};
    std::atomic<bool> source_finished_{false};
    std::atomic<bool> playback_finished_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> buffer_switches_{0};
    std::atomic<bool> paused_{false};
    std::atomic<SeekState> seek_state_{SeekState::None};
    std::atomic<std::uint64_t> seek_target_{0};          // control thread -> worker
    std::atomic<std::uint64_t> pending_output_position_{0}; // worker -> callback
    // Only the ASIO callback writes this, so plain relaxed loads are enough.
    std::atomic<std::uint64_t> output_position_frames_{0};
    std::uint64_t total_output_frames_ = 0;
    EngineStartFailure start_failure_ = EngineStartFailure::None;
    std::thread source_worker_;
};

} // namespace wasio
