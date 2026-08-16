#include "playback/ring_buffer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace wasio {

SpscByteRing::SpscByteRing(std::size_t capacity_bytes) : buffer_(capacity_bytes)
{
    if (capacity_bytes == 0) {
        throw std::invalid_argument("SpscByteRing capacity must be non-zero");
    }
}

std::size_t SpscByteRing::available_to_read() const
{
    const auto write = write_position_.load(std::memory_order_acquire);
    const auto read = read_position_.load(std::memory_order_relaxed);
    return static_cast<std::size_t>(write - read);
}

std::size_t SpscByteRing::available_to_write() const
{
    return capacity() - available_to_read();
}

std::size_t SpscByteRing::write(const std::uint8_t* source, std::size_t bytes)
{
    const auto write = write_position_.load(std::memory_order_relaxed);
    const auto read = read_position_.load(std::memory_order_acquire);
    const std::size_t count = std::min(bytes, capacity() - static_cast<std::size_t>(write - read));
    const std::size_t offset = static_cast<std::size_t>(write % capacity());
    const std::size_t first = std::min(count, capacity() - offset);
    std::memcpy(buffer_.data() + offset, source, first);
    if (count > first) {
        std::memcpy(buffer_.data(), source + first, count - first);
    }
    write_position_.store(write + count, std::memory_order_release);
    return count;
}

std::size_t SpscByteRing::read(std::uint8_t* destination, std::size_t bytes)
{
    const auto read = read_position_.load(std::memory_order_relaxed);
    const auto write = write_position_.load(std::memory_order_acquire);
    const std::size_t count = std::min(bytes, static_cast<std::size_t>(write - read));
    const std::size_t offset = static_cast<std::size_t>(read % capacity());
    const std::size_t first = std::min(count, capacity() - offset);
    std::memcpy(destination, buffer_.data() + offset, first);
    if (count > first) {
        std::memcpy(destination + first, buffer_.data(), count - first);
    }
    read_position_.store(read + count, std::memory_order_release);
    return count;
}

void SpscByteRing::clear()
{
    const auto write = write_position_.load(std::memory_order_acquire);
    read_position_.store(write, std::memory_order_release);
}

} // namespace wasio
