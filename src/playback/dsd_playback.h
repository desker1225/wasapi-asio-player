#pragma once

#include "asio/asio_session.h"
#include "playback/ring_buffer.h"
#include "playback/playback_engine.h"
#include "playback/seek_state.h"
#include "sources/dsd_source.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace wasio {

enum class DsdOutputMode {
    Native,
    DoP,
};

struct DsdPlaybackConfig {
    std::string driver_name;
    std::unique_ptr<DsdSource> source;
    DsdOutputMode mode = DsdOutputMode::Native;
    std::size_t ring_frames = 8192;
    long buffer_size = 0;
};

class DsdPlayback final {
public:
    DsdPlayback() = default;
    ~DsdPlayback();

    DsdPlayback(const DsdPlayback&) = delete;
    DsdPlayback& operator=(const DsdPlayback&) = delete;

    bool start(DsdPlaybackConfig config, std::string* error_message);
    EngineStartFailure start_failure() const { return start_failure_; }
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    bool source_finished() const { return source_finished_.load(std::memory_order_acquire); }
    bool playback_finished() const { return playback_finished_.load(std::memory_order_acquire); }
    std::uint64_t underrun_count() const { return underruns_.load(std::memory_order_relaxed); }
    std::uint64_t buffer_switch_count() const { return buffer_switches_.load(std::memory_order_relaxed); }
    std::uint64_t dsd_sample_rate() const { return dsd_sample_rate_; }
    std::uint64_t output_sample_rate() const { return output_sample_rate_; }
    DsdOutputMode mode() const { return mode_; }

    // ---- transport ----
    // Paused output is DSD/DoP silence, not zeros, and the DoP marker keeps
    // alternating, so the DAC stays locked in DSD mode across a pause.
    void set_paused(bool paused) { paused_.store(paused, std::memory_order_release); }
    bool is_paused() const { return paused_.load(std::memory_order_acquire); }
    std::uint64_t output_position_frames() const
    {
        return output_position_frames_.load(std::memory_order_relaxed);
    }
    std::uint64_t total_output_frames() const { return total_output_frames_; }
    // Frames per second for output_position_frames()/total_output_frames(), so
    // position_seconds = output_position_frames() / output_frame_rate().
    //
    // This is NOT output_sample_rate() in Native DSD mode: ASIO takes the raw
    // DSD bit rate as the device sample rate, but one output frame carries a
    // 32-bit word, so the frame rate is the DSD rate / 32. Each source frame is
    // 8 DSD bits per channel and source_frames_per_output_ of them make one
    // output frame, which covers Native (/32) and DoP (/16) with one formula.
    double output_frame_rate() const
    {
        if (source_frames_per_output_ == 0) return 0.0;
        return static_cast<double>(dsd_sample_rate_) /
               (8.0 * static_cast<double>(source_frames_per_output_));
    }
    // Native DSD packs 4 source bytes per 32-bit word, DoP packs 2.
    std::size_t source_frames_per_output() const { return source_frames_per_output_; }
    // Call from the control thread only.
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
    void fill_dsd_silence(std::uint8_t* destination, std::size_t bytes);
    // Realtime side. Fills callback_scratch_ with DSD silence and returns true
    // when pause or a pending seek owns this period.
    bool handle_transport(std::size_t requested_bytes);
    // Worker side. Returns true when it handled a seek or is parked for one.
    bool worker_handle_seek();

    AsioSession session_;
    std::unique_ptr<DsdSource> source_;
    std::unique_ptr<SpscByteRing> ring_;
    DsdOutputMode mode_ = DsdOutputMode::Native;
    std::uint64_t dsd_sample_rate_ = 0;
    std::uint64_t output_sample_rate_ = 0;
    long buffer_size_ = 0;
    long callback_frames_ = 0;
    long output_channels_ = 0;
    std::size_t output_bytes_per_channel_ = 0;
    std::size_t source_frames_per_output_ = 0;
    ASIOSampleType output_sample_type_ = ASIOSTInt32LSB;
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
    std::atomic<std::uint64_t> seek_target_{0};             // control thread -> worker
    std::atomic<std::uint64_t> pending_output_position_{0}; // worker -> callback
    // Only the ASIO callback writes this, so plain relaxed loads are enough.
    std::atomic<std::uint64_t> output_position_frames_{0};
    std::uint64_t total_output_frames_ = 0;
    EngineStartFailure start_failure_ = EngineStartFailure::None;
    std::thread source_worker_;
    // ASIO callback thread owns the DoP marker phase. This keeps the marker
    // continuous even when it has to insert silence after an underrun.
    std::uint64_t callback_output_frames_ = 0;
};

} // namespace wasio
