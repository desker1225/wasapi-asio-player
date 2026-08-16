#pragma once

#include "asiosys.h"
#include "asio.h"
#include "asio/driver_registry.h"

#include <string>
#include <vector>

namespace wasio {

const char* asio_error_name(ASIOError error);
const char* asio_sample_type_name(ASIOSampleType type);

class AsioSession {
public:
    AsioSession();
    ~AsioSession();

    AsioSession(const AsioSession&) = delete;
    AsioSession& operator=(const AsioSession&) = delete;

    bool open(const std::string& driver_name, std::string* error_message);
    void close();
    bool is_open() const { return initialized_; }

    ASIOError get_channels(long* inputs, long* outputs) const;
    ASIOError get_buffer_size(long* min_size, long* max_size, long* preferred_size,
                              long* granularity) const;
    ASIOError get_sample_rate(ASIOSampleRate* rate) const;
    ASIOError can_sample_rate(double rate) const;
    ASIOError set_sample_rate(double rate) const;
    ASIOError future(long selector, void* parameters) const;
    ASIOError get_channel_info(ASIOChannelInfo* info) const;
    ASIOError control_panel() const;
    ASIOError create_buffers(ASIOBufferInfo* buffers, long channel_count, long buffer_size,
                             ASIOCallbacks* callbacks) const;
    ASIOError dispose_buffers() const;
    ASIOError start() const;
    ASIOError stop() const;
    ASIOError output_ready() const;

private:
    void* probe_window_ = nullptr;
    bool initialized_ = false;
    std::string current_driver_name_;
};

std::vector<DriverRecord> enumerate_drivers();

} // namespace wasio
