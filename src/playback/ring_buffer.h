#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wasio {

// Single-producer/single-consumer byte ring. The producer is the source worker;
// the consumer is the ASIO callback. No locks or allocations are used in read/write.
class SpscByteRing {
public:
    explicit SpscByteRing(std::size_t capacity_bytes);

    SpscByteRing(const SpscByteRing&) = delete;
    SpscByteRing& operator=(const SpscByteRing&) = delete;

    std::size_t capacity() const { return buffer_.size(); }
    std::size_t available_to_read() const;
    std::size_t available_to_write() const;
    std::size_t write(const std::uint8_t* source, std::size_t bytes);
    std::size_t read(std::uint8_t* destination, std::size_t bytes);
    void clear();

private:
    std::vector<std::uint8_t> buffer_;
    std::atomic<std::uint64_t> read_position_{0};
    std::atomic<std::uint64_t> write_position_{0};
};

} // namespace wasio
