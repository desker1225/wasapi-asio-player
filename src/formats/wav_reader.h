#pragma once

#include "sources/audio_source.h"

#include <cstdint>
#include <fstream>
#include <string>

namespace wasio {

class WavPcmSource final : public AudioSource {
public:
    explicit WavPcmSource(const std::string& path);

    bool valid() const { return valid_; }
    const std::string& error() const { return error_; }
    const AudioFormat& format() const override { return format_; }
    std::size_t read_frames(std::uint8_t* destination, std::size_t frames) override;
    bool finished() const override;

    std::uint64_t total_frames() const override;
    std::uint64_t position_frames() const override;
    bool seek_frames(std::uint64_t frame) override;

private:
    bool parse();
    static std::uint16_t read_u16(const std::uint8_t* bytes);
    static std::uint32_t read_u32(const std::uint8_t* bytes);

    std::ifstream file_;
    AudioFormat format_;
    std::uint64_t data_offset_ = 0;
    std::uint64_t data_total_ = 0;
    std::uint64_t data_remaining_ = 0;
    bool valid_ = false;
    std::string error_;
};

} // namespace wasio
