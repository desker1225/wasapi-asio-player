#pragma once

#include <cstddef>
#include <cstdint>

namespace wasio {

class DsdPacker final {
public:
    // Canonical raw source is LSB1. Native ASIO DSD output is MSB1.
    static void native_msb1(const std::uint8_t* lsb_bytes, std::uint8_t* output,
                            std::size_t byte_count);
    // DoP transmits t0 first from the MSB side of its 16-bit payload. Convert
    // the canonical LSB1 bytes and place the older byte above the newer byte.
    // Int32LSB carries the resulting 24-bit DoP word in its upper 24 bits.
    static void dop_int32(const std::uint8_t* dsd_bytes, std::uint8_t marker,
                          std::uint8_t* output);
    static void dop_int24(const std::uint8_t* dsd_bytes, std::uint8_t marker,
                          std::uint8_t* output);
    // Native DSD in a 4-byte subslot (a USB Audio Class alt-setting declaring
    // bSubslotSize=4 / bBitResolution=32).
    // Four canonical LSB1 bytes become one 32-bit word: byte order stays in
    // transmission order (oldest byte at the lowest address, which is the first
    // byte on the wire) and every byte is converted to MSB1, matching the
    // existing ASIO Native DSD container.
    static void native_word32(const std::uint8_t* dsd_bytes, std::uint8_t* output);
    static std::uint8_t reverse_bits(std::uint8_t value);
};

} // namespace wasio
