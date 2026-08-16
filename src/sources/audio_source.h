#pragma once

#include "audio/audio_format.h"

#include <cstddef>
#include <cstdint>

namespace wasio {

class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual const AudioFormat& format() const = 0;
    virtual std::size_t read_frames(std::uint8_t* destination, std::size_t frames) = 0;
    virtual bool finished() const = 0;

    virtual std::uint64_t total_frames() const = 0;
    virtual std::uint64_t position_frames() const = 0;
    // Seeking to total_frames() is legal and leaves the source finished().
    // Returns false without moving the position if the frame is out of range
    // or the underlying seek fails.
    virtual bool seek_frames(std::uint64_t frame) = 0;
};

} // namespace wasio
