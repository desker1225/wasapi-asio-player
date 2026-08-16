#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wasio {

struct WasapiProbeReport;

// One active WASAPI render endpoint as reported by IMMDeviceEnumerator. The
// UTF-8 name exists so the CLI can match --endpoint without wide-string
// plumbing; the wide name is kept for console-code-page output.
struct WasapiEndpointRecord {
    std::wstring id;
    std::wstring wide_name;
    std::string name;
    bool is_default = false;
};

std::vector<WasapiEndpointRecord> enumerate_wasapi_endpoints();

// Converts a wide string to the given code page (CP_UTF8 for matching,
// GetConsoleOutputCP() for printing).
std::string narrow_string(const std::wstring& value, unsigned int code_page);

enum class WasapiOutputMode {
    Pcm,
    DoP,
    // Implemented but never selected: dsd_mode_supported() in
    // player/engine_factory.h refuses WASAPI Native DSD, because its container
    // is indistinguishable from 32-bit PCM and there is no way to confirm the
    // device really entered DSD mode. Kept so the path is ready if that is ever
    // resolved; nothing in the shipped players reaches it.
    NativeDsd,
};

const char* wasapi_output_mode_name(WasapiOutputMode mode);
const char* wasapi_error_name(HRESULT result);

// WAVE_FORMAT_EXTENSIBLE PCM subformat GUID, shared by negotiate_format() and
// wasapi_probe's exclusive-mode format sweep.
extern const GUID kSubformatPcm;

// Builds a WAVEFORMATEXTENSIBLE for the given shape. Shared by
// negotiate_format() and wasapi_probe's format sweep.
WAVEFORMATEXTENSIBLE make_extensible(double sample_rate, std::uint16_t channels,
                                     std::uint16_t container_bits, std::uint16_t valid_bits,
                                     const GUID& subformat);

// Human-readable summary of a WAVEFORMATEX(TENSIBLE). Shared by
// negotiate_format()/initialize_exclusive() error messages and wasapi_probe's
// mix-format report.
std::string describe_waveformat(const WAVEFORMATEX* format);

struct WasapiFormatRequest {
    WasapiOutputMode mode = WasapiOutputMode::Pcm;
    // Frames per second handed to WASAPI. For DoP this is the PCM carrier rate
    // (DSD rate / 16); for Native DSD it is the 32-bit word rate (DSD rate / 32).
    double sample_rate = 48000.0;
    std::uint16_t channels = 2;
    // Zero lets negotiate_format() pick the container width for the mode.
    std::uint16_t preferred_container_bits = 0;
};

struct WasapiNegotiatedFormat {
    WAVEFORMATEXTENSIBLE format = {};
    std::uint16_t container_bits = 0;
    std::uint16_t valid_bits = 0;
    std::uint16_t channels = 0;
    double sample_rate = 0.0;
    std::string description;
    bool valid = false;

    std::size_t bytes_per_sample() const { return container_bits / 8u; }
    std::size_t bytes_per_frame() const { return channels * bytes_per_sample(); }
};

// Mirrors AsioSession: open()/close()/is_open() plus a self-contained probe().
// Everything here runs on the caller's thread; the render thread only uses
// event_handle()/render_client() after initialize_exclusive() succeeded.
class WasapiSession {
public:
    WasapiSession();
    ~WasapiSession();

    WasapiSession(const WasapiSession&) = delete;
    WasapiSession& operator=(const WasapiSession&) = delete;

    // An empty name selects the first active render endpoint. A non-empty name
    // matches the endpoint id exactly or the friendly name case-insensitively
    // as a substring.
    bool open(const std::string& endpoint_name, std::string* error_message);
    void close();
    bool is_open() const { return client_ != nullptr; }

    // The one place that decides which WAVEFORMATEXTENSIBLE reaches the driver.
    // A USB Audio Class device's 32-bit PCM and Native DSD alt-settings can
    // declare the same subslot size and bit resolution, so this is the method
    // to change if the disambiguation mechanism turns out to be something else.
    bool negotiate_format(const WasapiFormatRequest& request,
                          WasapiNegotiatedFormat* result,
                          std::string* error_message) const;
    HRESULT is_format_supported(const WAVEFORMATEXTENSIBLE& format) const;

    bool initialize_exclusive(const WasapiNegotiatedFormat& format,
                              std::string* error_message);
    bool start(std::string* error_message);
    void stop();

    HANDLE event_handle() const { return event_; }
    IAudioRenderClient* render_client() const { return render_client_; }
    std::uint32_t buffer_frame_count() const { return buffer_frame_count_; }
    double stream_period_ms() const { return stream_period_ms_; }
    const WasapiEndpointRecord& endpoint() const { return endpoint_; }
    const WasapiNegotiatedFormat& format() const { return format_; }

    // Defined in wasapi_probe.cpp: opens the endpoint, enumerates the
    // exclusive-mode format grid via IAudioClient, closes. Declared as a
    // friend (rather than a public accessor for client_) so probe stays the
    // only thing outside this class that reaches into IAudioClient directly.
    friend WasapiProbeReport probe_wasapi_session(WasapiSession& session,
                                                   const std::string& endpoint_name);

private:
    bool activate_client(std::string* error_message);
    void release_client();

    bool com_initialized_ = false;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_client_ = nullptr;
    HANDLE event_ = nullptr;
    std::uint32_t buffer_frame_count_ = 0;
    double stream_period_ms_ = 0.0;
    bool started_ = false;
    WasapiEndpointRecord endpoint_;
    WasapiNegotiatedFormat format_;
};

} // namespace wasio
