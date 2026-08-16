#pragma once

#include "sources/dsd_source.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace wasio {

class DsfDsdSource final : public DsdSource {
public:
    explicit DsfDsdSource(const std::string& path);

    bool valid() const { return valid_; }
    const std::string& error() const { return error_; }
    const DsdFormat& format() const override { return format_; }
    std::size_t read_frames(std::uint8_t* destination, std::size_t frames) override;
    bool finished() const override;

    std::uint64_t total_frames() const override;
    std::uint64_t position_frames() const override;
    bool seek_frames(std::uint64_t frame) override;

private:
    bool parse();
    bool load_block(std::size_t channel, std::uint64_t block);
    static std::uint32_t le32(const std::uint8_t* bytes);
    static std::uint64_t le64(const std::uint8_t* bytes);

    std::ifstream file_;
    DsdFormat format_;
    std::uint64_t sample_count_ = 0;
    std::uint64_t bytes_per_channel_ = 0;
    std::uint32_t block_size_ = 0;
    std::uint64_t data_offset_ = 0;
    std::uint64_t frame_position_ = 0;
    std::vector<std::uint64_t> cached_blocks_;
    std::vector<std::vector<std::uint8_t>> block_data_;
    bool valid_ = false;
    std::string error_;
};

} // namespace wasio
