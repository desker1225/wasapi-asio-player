#include "formats/wav_reader.h"

#include "formats/utf8_file.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace wasio {

WavPcmSource::WavPcmSource(const std::string& path) : file_(open_utf8_ifstream(path, std::ios::binary))
{
    if (!file_) {
        error_ = "cannot open WAV file";
        return;
    }
    valid_ = parse();
}

std::uint16_t WavPcmSource::read_u16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

std::uint32_t WavPcmSource::read_u32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0] | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                      (static_cast<std::uint32_t>(bytes[2]) << 16) |
                                      (static_cast<std::uint32_t>(bytes[3]) << 24));
}

bool WavPcmSource::parse()
{
    std::array<std::uint8_t, 12> header = {};
    if (!file_.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        std::memcmp(header.data(), "RIFF", 4) != 0 || std::memcmp(header.data() + 8, "WAVE", 4) != 0) {
        error_ = "not a RIFF/WAVE file";
        return false;
    }

    bool found_fmt = false;
    bool found_data = false;
    while (file_ && (!found_fmt || !found_data)) {
        std::array<std::uint8_t, 8> chunk = {};
        if (!file_.read(reinterpret_cast<char*>(chunk.data()), chunk.size())) break;
        const std::uint32_t size = read_u32(chunk.data() + 4);
        const std::streamoff chunk_start = file_.tellg();
        if (std::memcmp(chunk.data(), "fmt ", 4) == 0) {
            std::vector<std::uint8_t> fmt(size);
            if (size < 16 || !file_.read(reinterpret_cast<char*>(fmt.data()), size)) {
                error_ = "truncated fmt chunk";
                return false;
            }
            const auto format_tag = read_u16(fmt.data());
            const auto channels = read_u16(fmt.data() + 2);
            const auto sample_rate = read_u32(fmt.data() + 4);
            const auto bits = read_u16(fmt.data() + 14);
            if (format_tag != 1 || channels == 0 || sample_rate == 0 ||
                (bits != 16 && bits != 24 && bits != 32)) {
                error_ = "WAV must be integer PCM 16/24/32-bit";
                return false;
            }
            format_.sample_rate = static_cast<double>(sample_rate);
            format_.channels = channels;
            format_.encoding = bits == 16 ? PcmEncoding::Int16
                                          : (bits == 24 ? PcmEncoding::Int24 : PcmEncoding::Int32);
            found_fmt = true;
        } else if (std::memcmp(chunk.data(), "data", 4) == 0) {
            data_total_ = size;
            data_remaining_ = size;
            data_offset_ = static_cast<std::uint64_t>(file_.tellg());
            found_data = true;
            file_.seekg(size, std::ios::cur);
        } else {
            file_.seekg(size, std::ios::cur);
        }
        if ((size & 1u) != 0) file_.seekg(1, std::ios::cur);
        if (file_.tellg() < chunk_start) break;
    }

    if (!found_fmt || !found_data || !format_.valid()) {
        error_ = "WAV fmt/data chunk missing";
        return false;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(data_offset_), std::ios::beg);
    return true;
}

std::size_t WavPcmSource::read_frames(std::uint8_t* destination, std::size_t frames)
{
    // Hoisted and checked explicitly: bytes_per_frame() has a zero path, and a
    // "data_remaining_ < 0" test would not have caught it before the divisions
    // below. valid_ already implies a non-zero frame, so this only makes the
    // guard say what it always meant.
    const std::size_t frame_bytes = format_.bytes_per_frame();
    if (!valid_ || frame_bytes == 0 || data_remaining_ < frame_bytes) return 0;
    const std::size_t max_frames =
        static_cast<std::size_t>(std::min<std::uint64_t>(frames, data_remaining_ / frame_bytes));
    const std::size_t bytes = max_frames * frame_bytes;
    if (!file_.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(bytes))) {
        const auto actual = static_cast<std::size_t>(file_.gcount());
        data_remaining_ -= std::min<std::uint64_t>(data_remaining_, actual);
        return actual / frame_bytes;
    }
    data_remaining_ -= bytes;
    return max_frames;
}

bool WavPcmSource::finished() const
{
    const std::size_t frame_bytes = format_.bytes_per_frame();
    return !valid_ || frame_bytes == 0 || data_remaining_ < frame_bytes;
}

std::uint64_t WavPcmSource::total_frames() const
{
    const std::size_t frame_bytes = format_.bytes_per_frame();
    if (!valid_ || frame_bytes == 0) return 0;
    return data_total_ / frame_bytes;
}

std::uint64_t WavPcmSource::position_frames() const
{
    const std::size_t frame_bytes = format_.bytes_per_frame();
    if (!valid_ || frame_bytes == 0) return 0;
    return (data_total_ - data_remaining_) / frame_bytes;
}

bool WavPcmSource::seek_frames(std::uint64_t frame)
{
    if (!valid_ || frame > total_frames()) return false;
    const std::uint64_t byte_offset = frame * format_.bytes_per_frame();
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(data_offset_ + byte_offset), std::ios::beg);
    if (!file_) {
        // Restore the stream to the old position so a failed seek is a no-op.
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(data_offset_ + (data_total_ - data_remaining_)),
                    std::ios::beg);
        return false;
    }
    data_remaining_ = data_total_ - byte_offset;
    return true;
}

} // namespace wasio
