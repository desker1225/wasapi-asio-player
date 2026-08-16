#pragma once

#include "player/engine_factory.h"
#include "player/playlist.h"

#include <cstdint>
#include <memory>
#include <string>

namespace wasio {

enum class PlayerState {
    Stopped,
    Playing,
    Paused,
};

const char* player_state_name(PlayerState state);

// Why the last operation failed, so the CLI can map it onto the exit codes in
// README.md without parsing message text.
enum class PlayerErrorKind {
    None,
    DeviceNotFound,          // 2
    DeviceOpenFailed,        // 3, includes AUDCLNT_E_DEVICE_IN_USE
    FileFailed,              // 4
    FormatNegotiationFailed, // 5
    DoP512Rejected,          // 6
    PlaybackInterrupted,     // 7, the render thread stopped on its own
};

// One snapshot of everything a UI needs to draw a frame, so GUI and CLI never
// have to reach into the engine themselves.
struct PlayerStatus {
    PlayerState state = PlayerState::Stopped;
    std::size_t track_index = Playlist::kInvalidIndex;
    std::string track_name;
    double position_seconds = 0.0;
    double duration_seconds = 0.0;
    std::string format_description;
    std::uint64_t callback_count = 0;
    std::uint64_t underrun_count = 0;
    // Set when something went wrong; cleared by the next successful play().
    std::string error;
    PlayerErrorKind error_kind = PlayerErrorKind::None;
};

// The single operation surface for GUI and CLI. It owns the
// playlist, the device choice and the current engine; nothing above it talks to
// a playback class directly.
class PlayerController {
public:
    // The default factory builds real ASIO/WASAPI engines. Tests inject a fake
    // one to drive the track-change state machine without hardware.
    explicit PlayerController(EngineFactory factory = nullptr);
    ~PlayerController();

    PlayerController(const PlayerController&) = delete;
    PlayerController& operator=(const PlayerController&) = delete;

    Playlist& playlist() { return playlist_; }
    const Playlist& playlist() const { return playlist_; }

    void set_backend(PlaybackBackend backend) { backend_ = backend; }
    PlaybackBackend backend() const { return backend_; }
    void set_device(std::string device) { device_ = std::move(device); }
    const std::string& device() const { return device_; }
    void set_dsd_mode(DsdOutputMode mode) { dsd_mode_ = mode; }
    DsdOutputMode dsd_mode() const { return dsd_mode_; }

    // Starts `index`, replacing whatever is playing. Returns false and records
    // the reason in status().error when the track or device cannot be used.
    bool play(std::size_t index);
    void pause();
    void resume();
    void stop();
    // Skip to the neighbouring track, honouring shuffle. Repeat::One does not
    // apply here: a deliberate skip always moves.
    bool next();
    bool previous();
    bool seek(double seconds);

    // Call from the GUI timer or the CLI loop. Detects the end of a track and
    // advances per Repeat/Shuffle, then returns the current snapshot.
    PlayerStatus poll();
    PlayerStatus status() const;

private:
    bool start_track(std::size_t index, bool automatic);
    void release_engine();

    EngineFactory factory_;
    Playlist playlist_;
    std::unique_ptr<IPlaybackEngine> engine_;
    PlaybackBackend backend_ = PlaybackBackend::Wasapi;
    std::string device_;
    DsdOutputMode dsd_mode_ = DsdOutputMode::DoP;
    PlayerState state_ = PlayerState::Stopped;
    std::string error_;
    PlayerErrorKind error_kind_ = PlayerErrorKind::None;
    // Kept so status() still reports the last track after it finished.
    std::string track_name_;
    std::string format_description_;
    std::uint64_t last_callback_count_ = 0;
    std::uint64_t last_underrun_count_ = 0;
    // The render thread's own failure is only readable once the engine has been
    // stopped and joined, so it is captured at release time.
    std::string last_engine_error_;
};

} // namespace wasio
