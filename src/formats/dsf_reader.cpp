#include "formats/dsf_reader.h"

#include "formats/utf8_file.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace wasio {

DsfDsdSource::DsfDsdSource(const std::string& path) : file_(open_utf8_ifstream(path, std::ios::binary))
{
    if (!file_) {
        error_ = "cannot open DSF file";
        return;
    }
    valid_ = parse();
}

std::uint32_t DsfDsdSource::le32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0] | (bytes[1] << 8) |
                                      (bytes[2] << 16) | (bytes[3] << 24));
}

std::uint64_t DsfDsdSource::le64(const std::uint8_t* bytes)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | bytes[i];
    return value;
}

bool DsfDsdSource::parse()
{
    // Unlike DFF/IFF, every DSF chunk size (including this 28-byte master
    // chunk) counts its own 12-byte id+size header, so offsets are absolute
    // from each chunk's start rather than relative to the payload.
    std::array<std::uint8_t, 28> header = {};
    if (!file_.read(reinterpret_cast<char*>(header.data()), 28) ||
        std::memcmp(header.data(), "DSD ", 4) != 0) {
        error_ = "not a DSF file";
        return false;
    }

    bool found_fmt = false;
    bool found_data = false;
    while (file_ && (!found_fmt || !found_data)) {
        const std::streamoff chunk_start = file_.tellg();
        std::array<std::uint8_t, 12> chunk = {};
        if (!file_.read(reinterpret_cast<char*>(chunk.data()), 12)) break;
        const std::uint64_t size = le64(chunk.data() + 4);
        if (size < 12) break;
        const std::streamoff next_chunk = chunk_start + static_cast<std::streamoff>(size);
        if (std::memcmp(chunk.data(), "fmt ", 4) == 0) {
            if (size < 52) {
                error_ = "truncated DSF fmt chunk";
                return false;
            }
            std::array<std::uint8_t, 40> fmt = {};
            if (!file_.read(reinterpret_cast<char*>(fmt.data()), fmt.size())) return false;
            const auto format_version = le32(fmt.data());
            const auto format_id = le32(fmt.data() + 4);
            const auto channels = le32(fmt.data() + 12);
            const auto sample_rate = le32(fmt.data() + 16);
            const auto bits_per_sample = le32(fmt.data() + 20);
            sample_count_ = le64(fmt.data() + 24);
            block_size_ = le32(fmt.data() + 32);
            if (format_version != 1 || format_id != 0 || channels == 0 || sample_rate == 0 ||
                bits_per_sample != 1 || block_size_ == 0) {
                error_ = "unsupported DSF format (requires 1-bit DSD)";
                return false;
            }
            format_.sample_rate = sample_rate;
            format_.channels = channels;
            found_fmt = true;
        } else if (std::memcmp(chunk.data(), "data", 4) == 0) {
            data_offset_ = static_cast<std::uint64_t>(file_.tellg());
            bytes_per_channel_ = (sample_count_ + 7) / 8;
            found_data = true;
        }
        file_.seekg(next_chunk, std::ios::beg);
    }
    if (!found_fmt || !found_data || !format_.valid() || sample_count_ == 0 || bytes_per_channel_ == 0) {
        error_ = "DSF is missing fmt/data or has invalid metadata";
        return false;
    }
    cached_blocks_.assign(format_.channels, UINT64_MAX);
    block_data_.resize(format_.channels);
    file_.clear();
    return true;
}

bool DsfDsdSource::load_block(std::size_t channel, std::uint64_t block)
{
    if (channel >= format_.channels || block >= (bytes_per_channel_ + block_size_ - 1) / block_size_) {
        return false;
    }
    if (cached_blocks_[channel] == block) return true;
    const std::uint64_t offset = data_offset_ +
        (block * format_.channels + channel) * static_cast<std::uint64_t>(block_size_);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    const std::size_t bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(block_size_, bytes_per_channel_ - block * block_size_));
    block_data_[channel].assign(block_size_, 0);
    if (!file_.read(reinterpret_cast<char*>(block_data_[channel].data()),
                    static_cast<std::streamsize>(bytes))) {
        return false;
    }
    cached_blocks_[channel] = block;
    return true;
}

std::size_t DsfDsdSource::read_frames(std::uint8_t* destination, std::size_t frames)
{
    if (!valid_ || frame_position_ >= bytes_per_channel_) return 0;
    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
        frames, bytes_per_channel_ - frame_position_));
    std::size_t produced = 0;
    for (; produced < count; ++produced) {
        const auto block = frame_position_ / block_size_;
        const auto offset = static_cast<std::size_t>(frame_position_ % block_size_);
        bool ok = true;
        for (std::size_t channel = 0; channel < format_.channels; ++channel) {
            ok = load_block(channel, block);
            if (!ok) break;
            destination[produced * format_.channels + channel] = block_data_[channel][offset];
        }
        if (!ok) break;
        ++frame_position_;
    }
    return produced;
}

bool DsfDsdSource::finished() const
{
    return !valid_ || frame_position_ >= bytes_per_channel_;
}

std::uint64_t DsfDsdSource::total_frames() const
{
    return valid_ ? bytes_per_channel_ : 0;
}

std::uint64_t DsfDsdSource::position_frames() const
{
    return valid_ ? frame_position_ : 0;
}

bool DsfDsdSource::seek_frames(std::uint64_t frame)
{
    if (!valid_ || frame > bytes_per_channel_) return false;
    // No file seek here: read_frames() addresses data through load_block(),
    // which reloads whenever the requested block differs from the cached one.
    frame_position_ = frame;
    return true;
}

} // namespace wasio
