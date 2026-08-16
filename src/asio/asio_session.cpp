#include "asio/asio_session.h"

#include "asiodrivers.h"

#include <windows.h>

#include <algorithm>

// The ASIO SDK owns this process-wide driver manager instance.
extern AsioDrivers* asioDrivers;

namespace wasio {
namespace {

LRESULT CALLBACK probe_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

HWND create_probe_window()
{
    constexpr const char* class_name = "AsioToolsProbeWindow";
    static ATOM window_class = 0;
    if (window_class == 0) {
        WNDCLASSA klass = {};
        klass.lpfnWndProc = probe_window_proc;
        klass.hInstance = GetModuleHandleA(nullptr);
        klass.lpszClassName = class_name;
        window_class = RegisterClassA(&klass);
        if (window_class == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return nullptr;
        }
    }
    return CreateWindowExA(0, class_name, "ASIO Probe", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, GetModuleHandleA(nullptr), nullptr);
}

} // namespace

const char* asio_error_name(ASIOError error)
{
    switch (error) {
    case ASE_OK: return "ASE_OK";
    case ASE_SUCCESS: return "ASE_SUCCESS";
    case ASE_NotPresent: return "ASE_NotPresent";
    case ASE_HWMalfunction: return "ASE_HWMalfunction";
    case ASE_InvalidParameter: return "ASE_InvalidParameter";
    case ASE_InvalidMode: return "ASE_InvalidMode";
    case ASE_SPNotAdvancing: return "ASE_SPNotAdvancing";
    case ASE_NoClock: return "ASE_NoClock";
    case ASE_NoMemory: return "ASE_NoMemory";
    default: return "ASIOError(unknown)";
    }
}

const char* asio_sample_type_name(ASIOSampleType type)
{
    switch (type) {
    case ASIOSTInt16MSB: return "Int16MSB";
    case ASIOSTInt16LSB: return "Int16LSB";
    case ASIOSTInt24MSB: return "Int24MSB";
    case ASIOSTInt24LSB: return "Int24LSB";
    case ASIOSTInt32MSB: return "Int32MSB";
    case ASIOSTInt32LSB: return "Int32LSB";
    case ASIOSTFloat32MSB: return "Float32MSB";
    case ASIOSTFloat32LSB: return "Float32LSB";
    case ASIOSTFloat64MSB: return "Float64MSB";
    case ASIOSTFloat64LSB: return "Float64LSB";
    case ASIOSTDSDInt8LSB1: return "DSDInt8LSB1";
    case ASIOSTDSDInt8MSB1: return "DSDInt8MSB1";
    case ASIOSTDSDInt8NER8: return "DSDInt8NER8";
    default: return "ASIOSampleType(unknown)";
    }
}

AsioSession::AsioSession()
    : probe_window_(create_probe_window())
{
}

AsioSession::~AsioSession()
{
    close();
    if (probe_window_ != nullptr) {
        DestroyWindow(static_cast<HWND>(probe_window_));
        probe_window_ = nullptr;
    }
}

bool AsioSession::open(const std::string& driver_name, std::string* error_message)
{
    close();
    if (asioDrivers == nullptr) {
        asioDrivers = new AsioDrivers();
    }
    if (asioDrivers == nullptr || !asioDrivers->loadDriver(const_cast<char*>(driver_name.c_str()))) {
        if (error_message != nullptr) {
            *error_message = "ASIO driver could not be loaded";
        }
        return false;
    }

    ASIODriverInfo info = {};
    info.asioVersion = 2;
    info.sysRef = probe_window_;
    const ASIOError error = ASIOInit(&info);
    if (error != ASE_OK) {
        if (error_message != nullptr) {
            *error_message = std::string(info.errorMessage) + " (" + asio_error_name(error) + ")";
        }
        asioDrivers->removeCurrentDriver();
        return false;
    }
    initialized_ = true;
    current_driver_name_ = driver_name;
    return true;
}

void AsioSession::close()
{
    if (initialized_) {
        ASIOExit();
        initialized_ = false;
    }
    if (asioDrivers != nullptr) {
        asioDrivers->removeCurrentDriver();
        delete asioDrivers;
        asioDrivers = nullptr;
    }
    current_driver_name_.clear();
}

ASIOError AsioSession::get_channels(long* inputs, long* outputs) const
{
    return initialized_ ? ASIOGetChannels(inputs, outputs) : ASE_NotPresent;
}

ASIOError AsioSession::get_buffer_size(long* min_size, long* max_size, long* preferred_size,
                                        long* granularity) const
{
    return initialized_ ? ASIOGetBufferSize(min_size, max_size, preferred_size, granularity)
                        : ASE_NotPresent;
}

ASIOError AsioSession::get_sample_rate(ASIOSampleRate* rate) const
{
    return initialized_ ? ASIOGetSampleRate(rate) : ASE_NotPresent;
}

ASIOError AsioSession::can_sample_rate(double rate) const
{
    return initialized_ ? ASIOCanSampleRate(rate) : ASE_NotPresent;
}

ASIOError AsioSession::set_sample_rate(double rate) const
{
    return initialized_ ? ASIOSetSampleRate(rate) : ASE_NotPresent;
}

ASIOError AsioSession::future(long selector, void* parameters) const
{
    return initialized_ ? ASIOFuture(selector, parameters) : ASE_NotPresent;
}

ASIOError AsioSession::get_channel_info(ASIOChannelInfo* info) const
{
    return initialized_ ? ASIOGetChannelInfo(info) : ASE_NotPresent;
}

ASIOError AsioSession::control_panel() const
{
    return initialized_ ? ASIOControlPanel() : ASE_NotPresent;
}

ASIOError AsioSession::create_buffers(ASIOBufferInfo* buffers, long channel_count,
                                      long buffer_size, ASIOCallbacks* callbacks) const
{
    return initialized_ ? ASIOCreateBuffers(buffers, channel_count, buffer_size, callbacks)
                        : ASE_NotPresent;
}

ASIOError AsioSession::dispose_buffers() const
{
    return initialized_ ? ASIODisposeBuffers() : ASE_NotPresent;
}

ASIOError AsioSession::start() const
{
    return initialized_ ? ASIOStart() : ASE_NotPresent;
}

ASIOError AsioSession::stop() const
{
    return initialized_ ? ASIOStop() : ASE_NotPresent;
}

ASIOError AsioSession::output_ready() const
{
    return initialized_ ? ASIOOutputReady() : ASE_NotPresent;
}

std::vector<DriverRecord> enumerate_drivers()
{
    return enumerate_registered_drivers();
}

} // namespace wasio
