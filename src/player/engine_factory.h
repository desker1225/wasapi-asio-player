#pragma once

#include "playback/dsd_playback.h"
#include "playback/playback_engine.h"
#include "player/track_info.h"

#include <functional>
#include <memory>
#include <string>

namespace wasio {

enum class PlaybackBackend {
    Asio,
    Wasapi,
};

const char* playback_backend_name(PlaybackBackend backend);

// What the caller wants played, before any device is touched.
struct EngineRequest {
    PlaybackBackend backend = PlaybackBackend::Asio;
    // ASIO driver name, or WASAPI endpoint id/friendly-name substring. Empty
    // means "first usable device" for both backends.
    std::string device;
    TrackInfo track;
    DsdOutputMode dsd_mode = DsdOutputMode::Native;
};

// The one place that maps a track onto a backend: PCM files get
// a PCM output, DSD files get Native DSD or DoP by preference, and DoP512 is
// refused outright. Returns nullptr with error_message set when the request
// cannot be satisfied. The engine is returned un-started.
using EngineFactory = std::function<std::unique_ptr<IPlaybackEngine>(const EngineRequest&,
                                                                     std::string* error_message)>;

std::unique_ptr<IPlaybackEngine> create_engine(const EngineRequest& request,
                                               std::string* error_message);

// Resolves an empty/partial device name to something the backend can open, so
// callers get "no such device" separately from "device would not start".
// ASIO has no default-driver concept, so an empty name picks the first usable
// x64 driver; WASAPI keeps an empty name, which its session reads as the first
// active render endpoint.
bool resolve_device(PlaybackBackend backend, std::string* device);

} // namespace wasio
