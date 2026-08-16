#include "formats/dff_reader.h"

#include "formats/utf8_file.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace wasio {
namespace {

bool id_is(const std::array<std::uint8_t, 4>& id, const char* text)
{
    return std::memcmp(id.data(), text, 4) == 0;
}

} // namespace

DffDsdSource::DffDsdSource(const std::string& path) : file_(open_utf8_ifstream(path, std::ios::binary))
{
    if (!file_) {
        error_ = "cannot open DFF file";
        return;
    }
    valid_ = parse();
}

std::uint16_t DffDsdSource::be16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

std::uint32_t DffDsdSource::be32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}

std::uint64_t DffDsdSource::be64(const std::uint8_t* bytes)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    return value;
}

std::uint8_t DffDsdSource::reverse_bits(std::uint8_t value)
{
    value = static_cast<std::uint8_t>((value >> 4) | (value << 4));
    value = static_cast<std::uint8_t>(((value & 0xcc) >> 2) | ((value & 0x33) << 2));
    return static_cast<std::uint8_t>(((value & 0xaa) >> 1) | ((value & 0x55) << 1));
}

bool DffDsdSource::parse()
{
    std::array<std::uint8_t, 16> header = {};
    if (!file_.read(reinterpret_cast<char*>(header.data()), 16) ||
        std::memcmp(header.data(), "FRM8", 4) != 0 ||
        std::memcmp(header.data() + 12, "DSD ", 4) != 0) {
        error_ = "not a DSDIFF FRM8 file";
        return false;
    }
    const std::uint64_t form_size = be64(header.data() + 4);
    const std::streamoff form_end = 16 + static_cast<std::streamoff>(form_size);
    bool found_rate = false;
    bool found_channels = false;
    while (file_ && file_.tellg() < form_end) {
        std::array<std::uint8_t, 12> chunk = {};
        if (!file_.read(reinterpret_cast<char*>(chunk.data()), 12)) break;
        const std::array<std::uint8_t, 4> id = {chunk[0], chunk[1], chunk[2], chunk[3]};
        const std::uint64_t size = be64(chunk.data() + 4);
        const std::streamoff payload = file_.tellg();
        if (id_is(id, "PROP")) {
            std::array<std::uint8_t, 4> property = {};
            if (!file_.read(reinterpret_cast<char*>(property.data()), 4)) break;
            std::uint64_t consumed = 4;
            while (consumed + 12 <= size) {
                std::array<std::uint8_t, 12> nested = {};
                if (!file_.read(reinterpret_cast<char*>(nested.data()), 12)) break;
                const std::array<std::uint8_t, 4> nested_id = {nested[0], nested[1], nested[2], nested[3]};
                const std::uint64_t nested_size = be64(nested.data() + 4);
                consumed += 12 + nested_size;
                if (id_is(nested_id, "FS  ") && nested_size >= 4) {
                    std::array<std::uint8_t, 4> rate = {};
                    file_.read(reinterpret_cast<char*>(rate.data()), 4);
                    format_.sample_rate = be32(rate.data());
                    found_rate = true;
                    if (nested_size > 4) file_.seekg(static_cast<std::streamoff>(nested_size - 4), std::ios::cur);
                } else if (id_is(nested_id, "CHNL") && nested_size >= 2) {
                    std::array<std::uint8_t, 2> channels = {};
                    file_.read(reinterpret_cast<char*>(channels.data()), 2);
                    format_.channels = be16(channels.data());
                    found_channels = format_.channels != 0;
                    if (nested_size > 2) file_.seekg(static_cast<std::streamoff>(nested_size - 2), std::ios::cur);
                } else {
                    file_.seekg(static_cast<std::streamoff>(nested_size), std::ios::cur);
                }
                if ((nested_size & 1u) != 0) file_.seekg(1, std::ios::cur);
            }
            file_.seekg(payload + static_cast<std::streamoff>(size), std::ios::beg);
        } else if (id_is(id, "DSD ")) {
            data_offset_ = static_cast<std::uint64_t>(payload);
            data_total_ = size;
            data_remaining_ = size;
            file_.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        } else {
            file_.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        }
        if ((size & 1u) != 0) file_.seekg(1, std::ios::cur);
    }
    if (!found_rate || !found_channels || data_offset_ == 0 || !format_.valid()) {
        error_ = "DFF is missing DSD rate, channels, or data";
        return false;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(data_offset_), std::ios::beg);
    return true;
}

std::size_t DffDsdSource::read_frames(std::uint8_t* destination, std::size_t frames)
{
    if (!valid_ || data_remaining_ < format_.channels) return 0;
    const std::size_t max_frames = static_cast<std::size_t>(
        std::min<std::uint64_t>(frames, data_remaining_ / format_.channels));
    const std::size_t bytes = max_frames * format_.channels;
    std::vector<std::uint8_t> raw(bytes);
    if (!file_.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(bytes))) {
        const auto actual = static_cast<std::size_t>(file_.gcount());
        data_remaining_ -= std::min<std::uint64_t>(data_remaining_, actual);
        for (std::size_t i = 0; i < actual; ++i) destination[i] = reverse_bits(raw[i]);
        return actual / format_.channels;
    }
    data_remaining_ -= bytes;
    for (std::size_t i = 0; i < bytes; ++i) destination[i] = reverse_bits(raw[i]);
    return max_frames;
}

bool DffDsdSource::finished() const
{
    return !valid_ || data_remaining_ < format_.channels;
}

std::uint64_t DffDsdSource::total_frames() const
{
    if (!valid_) return 0;
    return data_total_ / format_.channels;
}

std::uint64_t DffDsdSource::position_frames() const
{
    if (!valid_) return 0;
    return (data_total_ - data_remaining_) / format_.channels;
}

bool DffDsdSource::seek_frames(std::uint64_t frame)
{
    if (!valid_ || frame > total_frames()) return false;
    const std::uint64_t byte_offset = frame * format_.channels;
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
