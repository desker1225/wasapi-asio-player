#pragma once

#include <cstddef>
#include <cstdint>

namespace wasio {

enum class PcmEncoding {
    Int16,
    Int24,
    Int32,
};

struct AudioFormat {
    double sample_rate = 0.0;
    std::size_t channels = 0;
    PcmEncoding encoding = PcmEncoding::Int32;

    std::size_t bytes_per_sample() const
    {
        switch (encoding) {
        case PcmEncoding::Int16: return 2;
        case PcmEncoding::Int24: return 3;
        case PcmEncoding::Int32: return 4;
        }
        return 0;
    }

    std::size_t bytes_per_frame() const { return channels * bytes_per_sample(); }
    bool valid() const { return sample_rate > 0.0 && channels > 0 && bytes_per_sample() != 0; }
};

} // namespace wasio
