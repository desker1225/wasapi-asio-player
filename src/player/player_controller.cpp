#include "player/player_controller.h"

#include <algorithm>

namespace wasio {

const char* player_state_name(PlayerState state)
{
    switch (state) {
    case PlayerState::Stopped: return "stopped";
    case PlayerState::Playing: return "playing";
    case PlayerState::Paused: return "paused";
    }
    return "stopped";
}

PlayerController::PlayerController(EngineFactory factory)
    : factory_(factory ? std::move(factory) : EngineFactory(&create_engine))
{
}

PlayerController::~PlayerController()
{
    release_engine();
}

void PlayerController::release_engine()
{
    if (engine_) {
        // Keep the totals so the UI can still show them after the track ends.
        last_callback_count_ = engine_->callback_count();
        last_underrun_count_ = engine_->underrun_count();
        engine_->stop();
        // Only readable now: WasapiPlayback fills render_error() when the render
        // thread is joined, which stop() has just done.
        last_engine_error_ = engine_->error_message();
        engine_.reset();
        if (!last_engine_error_.empty() && error_.empty()) {
            error_ = last_engine_error_;
            error_kind_ = PlayerErrorKind::PlaybackInterrupted;
        }
    }
}

bool PlayerController::start_track(std::size_t index, bool automatic)
{
    if (index >= playlist_.size()) {
        error_ = "no such track in the playlist";
        error_kind_ = PlayerErrorKind::FileFailed;
        return false;
    }
    // A previous render-thread failure must not be mistaken for this track's.
    error_.clear();
    error_kind_ = PlayerErrorKind::None;
    release_engine();
    error_.clear();
    error_kind_ = PlayerErrorKind::None;

    EngineRequest request;
    request.backend = backend_;
    request.device = device_;
    request.track = playlist_.at(index);
    request.dsd_mode = dsd_mode_;
    // Resolving here rather than inside the engine keeps "no such device"
    // distinguishable from "the device refused to start".
    if (!resolve_device(request.backend, &request.device)) {
        error_ = std::string("no ") + playback_backend_name(request.backend) +
                 " device matches " + (device_.empty() ? "the default device" : device_);
        error_kind_ = PlayerErrorKind::DeviceNotFound;
        state_ = PlayerState::Stopped;
        return false;
    }
    // Classified here rather than from the factory's message: these two are
    // properties of the track and the DSD preference, both known up front.
    if (!request.track.valid) {
        error_ = request.track.error.empty() ? "the track could not be read" : request.track.error;
        error_kind_ = PlayerErrorKind::FileFailed;
        state_ = PlayerState::Stopped;
        return false;
    }
    std::string mode_error;
    if (request.track.kind == TrackKind::DsdFile &&
        !dsd_mode_supported(request.backend, request.dsd_mode, &mode_error)) {
        error_ = mode_error;
        error_kind_ = PlayerErrorKind::FormatNegotiationFailed;
        state_ = PlayerState::Stopped;
        return false;
    }
    if (request.track.kind == TrackKind::DsdFile && request.dsd_mode == DsdOutputMode::DoP &&
        request.track.sample_rate > static_cast<double>(kMaxDopDsdRate)) {
        error_ = "DoP512 is not supported: it requires a 1,411,200 Hz PCM host rate";
        error_kind_ = PlayerErrorKind::DoP512Rejected;
        state_ = PlayerState::Stopped;
        return false;
    }

    std::string message;
    auto engine = factory_(request, &message);
    if (!engine) {
        // The factory only gets this far on a reader failure.
        error_ = message.empty() ? "the track could not be played" : message;
        error_kind_ = PlayerErrorKind::FileFailed;
        state_ = PlayerState::Stopped;
        return false;
    }
    if (!engine->start(&message)) {
        error_ = message.empty() ? "playback could not start" : message;
        error_kind_ = engine->start_failure() == EngineStartFailure::FormatNegotiation
                          ? PlayerErrorKind::FormatNegotiationFailed
                          : PlayerErrorKind::DeviceOpenFailed;
        state_ = PlayerState::Stopped;
        return false;
    }

    engine_ = std::move(engine);
    playlist_.set_current(index);
    track_name_ = request.track.display_name;
    format_description_ = engine_->format_description();
    last_callback_count_ = 0;
    last_underrun_count_ = 0;
    last_engine_error_.clear();
    error_.clear();
    error_kind_ = PlayerErrorKind::None;
    state_ = PlayerState::Playing;
    (void)automatic;
    return true;
}

bool PlayerController::play(std::size_t index)
{
    return start_track(index, false);
}

void PlayerController::pause()
{
    if (!engine_ || state_ != PlayerState::Playing) return;
    engine_->set_paused(true);
    state_ = PlayerState::Paused;
}

void PlayerController::resume()
{
    if (!engine_ || state_ != PlayerState::Paused) return;
    engine_->set_paused(false);
    state_ = PlayerState::Playing;
}

void PlayerController::stop()
{
    release_engine();
    state_ = PlayerState::Stopped;
}

bool PlayerController::next()
{
    const std::size_t index = playlist_.next_index(false);
    if (index == Playlist::kInvalidIndex) return false;
    playlist_.next(false);
    return start_track(index, false);
}

bool PlayerController::previous()
{
    const std::size_t index = playlist_.previous_index();
    if (index == Playlist::kInvalidIndex) return false;
    playlist_.previous();
    return start_track(index, false);
}

bool PlayerController::seek(double seconds)
{
    if (!engine_) return false;
    const double rate = engine_->output_frame_rate();
    if (rate <= 0.0) return false;
    const double duration = static_cast<double>(engine_->total_output_frames()) / rate;
    const double clamped = std::max(0.0, std::min(seconds, duration));
    const auto output_frame = static_cast<std::uint64_t>(clamped * rate);
    return engine_->request_seek(output_frame * engine_->source_frames_per_output());
}

PlayerStatus PlayerController::poll()
{
    if (engine_ && state_ != PlayerState::Stopped && engine_->playback_finished()) {
        // The track ended on its own, so Repeat::One replays it and shuffle
        // picks the next entry in the current round.
        const std::size_t index = playlist_.next_index(true);
        if (index == Playlist::kInvalidIndex) {
            release_engine();
            state_ = PlayerState::Stopped;
        } else {
            playlist_.next(true);
            if (!start_track(index, true)) {
                release_engine();
                state_ = PlayerState::Stopped;
            }
        }
    }
    return status();
}

PlayerStatus PlayerController::status() const
{
    PlayerStatus snapshot;
    snapshot.state = state_;
    snapshot.track_index = playlist_.current_index();
    snapshot.track_name = track_name_;
    snapshot.format_description = format_description_;
    snapshot.error = error_;
    snapshot.error_kind = error_kind_;
    if (engine_) {
        const double rate = engine_->output_frame_rate();
        if (rate > 0.0) {
            snapshot.position_seconds = static_cast<double>(engine_->output_position_frames()) / rate;
            snapshot.duration_seconds = static_cast<double>(engine_->total_output_frames()) / rate;
        }
        snapshot.callback_count = engine_->callback_count();
        snapshot.underrun_count = engine_->underrun_count();
    } else {
        snapshot.callback_count = last_callback_count_;
        snapshot.underrun_count = last_underrun_count_;
    }
    return snapshot;
}

} // namespace wasio
