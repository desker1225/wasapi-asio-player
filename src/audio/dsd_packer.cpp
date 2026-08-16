#include "audio/dsd_packer.h"

namespace wasio {

std::uint8_t DsdPacker::reverse_bits(std::uint8_t value)
{
    value = static_cast<std::uint8_t>((value >> 4) | (value << 4));
    value = static_cast<std::uint8_t>(((value & 0xcc) >> 2) | ((value & 0x33) << 2));
    return static_cast<std::uint8_t>(((value & 0xaa) >> 1) | ((value & 0x55) << 1));
}

void DsdPacker::native_msb1(const std::uint8_t* lsb_bytes, std::uint8_t* output,
                            std::size_t byte_count)
{
    for (std::size_t index = 0; index < byte_count; ++index) {
        output[index] = reverse_bits(lsb_bytes[index]);
    }
}

void DsdPacker::dop_int32(const std::uint8_t* dsd_bytes, std::uint8_t marker,
                          std::uint8_t* output)
{
    output[0] = 0;
    output[1] = reverse_bits(dsd_bytes[1]);
    output[2] = reverse_bits(dsd_bytes[0]);
    output[3] = marker;
}

void DsdPacker::dop_int24(const std::uint8_t* dsd_bytes, std::uint8_t marker,
                          std::uint8_t* output)
{
    output[0] = reverse_bits(dsd_bytes[1]);
    output[1] = reverse_bits(dsd_bytes[0]);
    output[2] = marker;
}

void DsdPacker::native_word32(const std::uint8_t* dsd_bytes, std::uint8_t* output)
{
    output[0] = reverse_bits(dsd_bytes[0]);
    output[1] = reverse_bits(dsd_bytes[1]);
    output[2] = reverse_bits(dsd_bytes[2]);
    output[3] = reverse_bits(dsd_bytes[3]);
}

} // namespace wasio
