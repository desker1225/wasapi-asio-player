#pragma once

#include "audio/audio_format.h"

#include <cstdint>
#include <string>

namespace wasio {

enum class TrackKind {
    Unknown,
    PcmFile,
    DsdFile,
};

const char* track_kind_name(TrackKind kind);

// What the playlist needs to show a row without holding the file open.
struct TrackInfo {
    std::string path;         // UTF-8
    std::string display_name; // file name without directory
    TrackKind kind = TrackKind::Unknown;
    // For DSD this is the DSD bit rate (2,822,400 for DSD64), not a PCM rate.
    double sample_rate = 0.0;
    std::size_t channels = 0;
    PcmEncoding encoding = PcmEncoding::Int32; // only meaningful for PcmFile
    std::uint64_t total_frames = 0;
    double duration_seconds = 0.0;
    bool valid = false;
    std::string error;
};

// Opens the file, reads format and length, and closes it again, so a playlist
// can display a track without keeping a handle on it. The format is chosen by
// file extension (.wav / .dff / .dsf).
TrackInfo probe_track(const std::string& path);

} // namespace wasio
