#pragma once

#include <cstddef>
#include <cstdint>

namespace wasio {

// DoP carries 16 DSD bits inside each 24-bit PCM frame, so the carrier runs at
// the DSD rate / 16. DSD512 would need a 1,411,200 Hz carrier, which no current
// device offers, so DoP stops here and DSD512 has to go out as Native DSD.
// Refused rather than silently downgraded.
constexpr std::uint64_t kMaxDopDsdRate = 11289600;

struct DsdFormat {
    std::uint64_t sample_rate = 0; // DSD bit rate: 2,822,400 for DSD64
    std::size_t channels = 0;

    std::size_t bytes_per_frame() const { return channels; }
    bool valid() const { return sample_rate != 0 && channels != 0; }
};

class DsdSource {
public:
    virtual ~DsdSource() = default;
    virtual const DsdFormat& format() const = 0;
    // One frame is one packed DSD byte per channel (8 one-bit samples).
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
