#pragma once

#include "audio/audio_format.h"
#include "playback/ring_buffer.h"
#include "playback/playback_engine.h"
#include "playback/seek_state.h"
#include "sources/audio_source.h"
#include "sources/dsd_source.h"
#include "wasapi/wasapi_session.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace wasio {

struct WasapiPlaybackConfig {
    std::string endpoint_name; // empty selects the first active render endpoint
    WasapiOutputMode mode = WasapiOutputMode::Pcm;
    std::unique_ptr<AudioSource> pcm_source; // used when mode == Pcm
    std::unique_ptr<DsdSource> dsd_source;   // used when mode == DoP or NativeDsd
    std::size_t ring_frames = 16384;
};

// Mirrors PcmPlayback/DsdPlayback: a worker thread converts source frames into
// an SpscByteRing, and a realtime consumer drains the ring. The consumer here is
// an event-driven exclusive-mode render thread instead of a driver-invoked ASIO
// callback, but it keeps the same discipline: no file I/O, no allocation, and a
// silence fill plus underrun count whenever the ring runs short.
class WasapiPlayback final {
public:
    WasapiPlayback() = default;
    ~WasapiPlayback();

    WasapiPlayback(const WasapiPlayback&) = delete;
    WasapiPlayback& operator=(const WasapiPlayback&) = delete;

    bool start(WasapiPlaybackConfig config, std::string* error_message);
    EngineStartFailure start_failure() const { return start_failure_; }
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }
    bool source_finished() const { return source_finished_.load(std::memory_order_acquire); }
    bool playback_finished() const { return playback_finished_.load(std::memory_order_acquire); }
    std::uint64_t underrun_count() const { return underruns_.load(std::memory_order_relaxed); }
    std::uint64_t render_callback_count() const
    {
        return render_callbacks_.load(std::memory_order_relaxed);
    }
    // Same counter under the ASIO-side name so both backends report alike.
    std::uint64_t buffer_switch_count() const { return render_callback_count(); }

    WasapiOutputMode mode() const { return mode_; }
    std::uint64_t dsd_sample_rate() const { return dsd_sample_rate_; }
    double output_sample_rate() const { return output_sample_rate_; }
    std::uint32_t buffer_frame_count() const { return buffer_frame_count_; }
    const std::string& format_description() const { return format_description_; }
    const std::string& endpoint_name() const { return endpoint_name_; }
    // Non-empty if the render thread stopped early; only valid after stop().
    const std::string& render_error() const { return render_error_; }

    // ---- transport ----
    // The exclusive-mode stream stays open while paused: the render thread
    // writes silence without consuming the ring, counting an underrun, or
    // advancing the position. In DoP mode the marker keeps alternating.
    void set_paused(bool paused) { paused_.store(paused, std::memory_order_release); }
    bool is_paused() const { return paused_.load(std::memory_order_acquire); }
    std::uint64_t output_position_frames() const
    {
        return output_position_frames_.load(std::memory_order_relaxed);
    }
    std::uint64_t total_output_frames() const { return total_output_frames_; }
    // Frames per second for output_position_frames(). WASAPI always negotiates
    // the container rate itself (DSD/16 for DoP, DSD/32 for Native DSD), so
    // unlike ASIO Native DSD this is the same as output_sample_rate().
    double output_frame_rate() const { return output_sample_rate_; }
    std::size_t source_frames_per_output() const { return source_frames_per_output_; }
    // Call from the control thread only.
    bool request_seek(std::uint64_t source_frame);
    bool seek_in_progress() const
    {
        return seek_state_.load(std::memory_order_acquire) != SeekState::None;
    }

private:
    bool configure_output(std::string* error_message);
    void run_source_worker();
    void run_render_thread();
    void fill_render_buffer(std::uint8_t* buffer, std::uint32_t frames);
    void fill_output_silence(std::uint8_t* buffer, std::size_t offset, std::size_t bytes);
    // Render thread. Fills the device buffer with silence and returns true when
    // pause or a pending seek owns this period.
    bool handle_transport(std::uint8_t* buffer, std::size_t bytes);
    // Worker side. Returns true when it handled a seek or is parked for one.
    bool worker_handle_seek();
    bool seek_source(std::uint64_t frame);
    std::size_t convert_pcm(std::size_t frames);
    std::size_t convert_dsd(std::size_t output_frames);

    WasapiSession session_;
    std::unique_ptr<AudioSource> pcm_source_;
    std::unique_ptr<DsdSource> dsd_source_;
    std::unique_ptr<SpscByteRing> ring_;
    WasapiOutputMode mode_ = WasapiOutputMode::Pcm;

    std::uint64_t dsd_sample_rate_ = 0;
    double output_sample_rate_ = 0.0;
    std::uint32_t buffer_frame_count_ = 0;
    std::size_t output_channels_ = 0;
    std::size_t output_bytes_per_channel_ = 0;
    std::size_t output_frame_bytes_ = 0;
    std::size_t source_frames_per_output_ = 1;
    std::size_t worker_frames_ = 1024;
    std::uint16_t valid_bits_ = 0;
    std::string format_description_;
    std::string endpoint_name_;

    std::vector<std::uint8_t> source_scratch_;
    std::vector<std::uint8_t> conversion_scratch_;

    std::atomic<bool> stop_worker_{false};
    std::atomic<bool> stop_render_{false};
    std::atomic<bool> source_finished_{false};
    std::atomic<bool> playback_finished_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> render_callbacks_{0};
    std::atomic<bool> paused_{false};
    std::atomic<SeekState> seek_state_{SeekState::None};
    std::atomic<std::uint64_t> seek_target_{0};             // control thread -> worker
    std::atomic<std::uint64_t> pending_output_position_{0}; // worker -> render thread
    // Only the render thread writes this, so plain relaxed loads are enough.
    std::atomic<std::uint64_t> output_position_frames_{0};
    std::uint64_t total_output_frames_ = 0;
    EngineStartFailure start_failure_ = EngineStartFailure::None;
    // 0 = still starting, 1 = streaming, 2 = failed before streaming.
    std::atomic<int> render_state_{0};
    std::string render_error_;

    std::thread source_worker_;
    std::thread render_thread_;
    // The render thread owns the DoP marker phase so an underrun cannot break
    // the 0x05/0xfa alternation, exactly as the ASIO callback does.
    std::uint64_t render_output_frames_ = 0;
};

} // namespace wasio
