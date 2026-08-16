#include "wasapi/wasapi_session.h"

#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cwctype>
#include <string>

namespace wasio {

// WAVE_FORMAT_EXTENSIBLE subformat GUIDs follow the
// {0000xxxx-0000-0010-8000-00AA00389B71} pattern, where xxxx is the legacy
// wave format tag. They are spelled out here so this module does not have to
// pull ks.h/ksmedia.h into a /W4 /permissive- translation unit.
const GUID kSubformatPcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

namespace {

const GUID kSubformatIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// KSDATAFORMAT_SUBTYPE_IEC61937_DTS, ksmedia.h:3483 (WAVE_FORMAT_DTS = 0x0008).
// Type III / IEC 61937 passthrough is the spec-compliant way for a driver to
// say "this is not literal PCM", which is what the M3 firmware's alt-setting 5
// slot would use. Probed to find out whether that path exists on this endpoint.
const GUID kSubformatIec61937Dts = {
    0x00000008, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr DWORD kSpeakerFrontLeft = 0x00000001;
constexpr DWORD kSpeakerFrontRight = 0x00000002;
constexpr DWORD kSpeakerFrontCenter = 0x00000004;

struct ComApartment {
    ComApartment()
    {
        // S_FALSE still needs a matching CoUninitialize; RPC_E_CHANGED_MODE
        // means another apartment already owns this thread, so leave it alone.
        initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    }
    ~ComApartment()
    {
        if (initialized) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    bool initialized = false;
};

template <typename T>
void safe_release(T** pointer)
{
    if (pointer && *pointer) {
        (*pointer)->Release();
        *pointer = nullptr;
    }
}

DWORD channel_mask_for(std::uint16_t channels)
{
    if (channels == 1) return kSpeakerFrontCenter;
    if (channels == 2) return kSpeakerFrontLeft | kSpeakerFrontRight;
    return 0; // Let the driver decide for uncommon channel counts.
}

std::string guid_name(const GUID& value)
{
    if (IsEqualGUID(value, kSubformatPcm)) return "PCM";
    if (IsEqualGUID(value, kSubformatIeeeFloat)) return "IEEE_FLOAT";
    if (IsEqualGUID(value, kSubformatIec61937Dts)) return "IEC61937_DTS";
    wchar_t text[64] = {};
    if (StringFromGUID2(value, text, 64) > 0) {
        return narrow_string(text, CP_UTF8);
    }
    return "UNKNOWN";
}

std::string lowercase_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

struct BitCandidate {
    std::uint16_t container;
    std::uint16_t valid;
};

std::vector<BitCandidate> bit_candidates(WasapiOutputMode mode,
                                         std::uint16_t preferred_container_bits)
{
    std::vector<BitCandidate> candidates;
    switch (mode) {
    case WasapiOutputMode::Pcm:
        candidates = {{32, 32}, {32, 24}, {24, 24}, {16, 16}};
        break;
    case WasapiOutputMode::DoP:
        // DoP needs 24 payload bits plus the marker byte, so 16-bit containers
        // can never carry it.
        candidates = {{32, 32}, {32, 24}, {24, 24}};
        break;
    case WasapiOutputMode::NativeDsd:
        // Alt-setting 4 declares bSubslotSize=4 / bBitResolution=32, so only a
        // 4-byte container can reach it.
        candidates = {{32, 32}, {32, 24}};
        break;
    }
    if (preferred_container_bits != 0) {
        std::stable_partition(candidates.begin(), candidates.end(),
                              [preferred_container_bits](const BitCandidate& candidate) {
                                  return candidate.container == preferred_container_bits;
                              });
    }
    return candidates;
}

} // namespace

WAVEFORMATEXTENSIBLE make_extensible(double sample_rate, std::uint16_t channels,
                                     std::uint16_t container_bits, std::uint16_t valid_bits,
                                     const GUID& subformat)
{
    WAVEFORMATEXTENSIBLE format = {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = channels;
    format.Format.nSamplesPerSec = static_cast<DWORD>(sample_rate + 0.5);
    format.Format.wBitsPerSample = container_bits;
    format.Format.nBlockAlign =
        static_cast<WORD>(channels * static_cast<std::uint16_t>(container_bits / 8u));
    format.Format.nAvgBytesPerSec =
        format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = valid_bits;
    format.dwChannelMask = channel_mask_for(channels);
    format.SubFormat = subformat;
    return format;
}

std::string describe_waveformat(const WAVEFORMATEX* format)
{
    if (format == nullptr) return "<null>";
    std::string subformat = "tag " + std::to_string(format->wFormatTag);
    std::uint16_t valid_bits = format->wBitsPerSample;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        subformat = guid_name(extensible->SubFormat);
        valid_bits = extensible->Samples.wValidBitsPerSample;
    }
    return subformat + " " + std::to_string(format->nChannels) + " ch, " +
           std::to_string(format->nSamplesPerSec) + " Hz, " +
           std::to_string(format->wBitsPerSample) + "-bit container / " +
           std::to_string(valid_bits) + " valid bits";
}

std::string narrow_string(const std::wstring& value, unsigned int code_page)
{
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(code_page, 0, value.c_str(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(code_page, 0, value.c_str(), static_cast<int>(value.size()),
                        result.data(), needed, nullptr, nullptr);
    return result;
}

const char* wasapi_output_mode_name(WasapiOutputMode mode)
{
    switch (mode) {
    case WasapiOutputMode::Pcm: return "PCM";
    case WasapiOutputMode::DoP: return "DoP";
    case WasapiOutputMode::NativeDsd: return "Native DSD";
    }
    return "unknown";
}

const char* wasapi_error_name(HRESULT result)
{
    struct ErrorName {
        HRESULT code;
        const char* name;
    };
    static const ErrorName names[] = {
        {S_OK, "S_OK"},
        {S_FALSE, "S_FALSE"},
        {E_INVALIDARG, "E_INVALIDARG"},
        {E_POINTER, "E_POINTER"},
        {E_OUTOFMEMORY, "E_OUTOFMEMORY"},
        {E_NOINTERFACE, "E_NOINTERFACE"},
        {AUDCLNT_E_NOT_INITIALIZED, "AUDCLNT_E_NOT_INITIALIZED"},
        {AUDCLNT_E_ALREADY_INITIALIZED, "AUDCLNT_E_ALREADY_INITIALIZED"},
        {AUDCLNT_E_WRONG_ENDPOINT_TYPE, "AUDCLNT_E_WRONG_ENDPOINT_TYPE"},
        {AUDCLNT_E_DEVICE_INVALIDATED, "AUDCLNT_E_DEVICE_INVALIDATED"},
        {AUDCLNT_E_NOT_STOPPED, "AUDCLNT_E_NOT_STOPPED"},
        {AUDCLNT_E_BUFFER_TOO_LARGE, "AUDCLNT_E_BUFFER_TOO_LARGE"},
        {AUDCLNT_E_OUT_OF_ORDER, "AUDCLNT_E_OUT_OF_ORDER"},
        {AUDCLNT_E_UNSUPPORTED_FORMAT, "AUDCLNT_E_UNSUPPORTED_FORMAT"},
        {AUDCLNT_E_INVALID_SIZE, "AUDCLNT_E_INVALID_SIZE"},
        {AUDCLNT_E_DEVICE_IN_USE, "AUDCLNT_E_DEVICE_IN_USE"},
        {AUDCLNT_E_BUFFER_OPERATION_PENDING, "AUDCLNT_E_BUFFER_OPERATION_PENDING"},
        {AUDCLNT_E_THREAD_NOT_REGISTERED, "AUDCLNT_E_THREAD_NOT_REGISTERED"},
        {AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED, "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED"},
        {AUDCLNT_E_ENDPOINT_CREATE_FAILED, "AUDCLNT_E_ENDPOINT_CREATE_FAILED"},
        {AUDCLNT_E_SERVICE_NOT_RUNNING, "AUDCLNT_E_SERVICE_NOT_RUNNING"},
        {AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED, "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED"},
        {AUDCLNT_E_EXCLUSIVE_MODE_ONLY, "AUDCLNT_E_EXCLUSIVE_MODE_ONLY"},
        {AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL, "AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL"},
        {AUDCLNT_E_EVENTHANDLE_NOT_SET, "AUDCLNT_E_EVENTHANDLE_NOT_SET"},
        {AUDCLNT_E_INCORRECT_BUFFER_SIZE, "AUDCLNT_E_INCORRECT_BUFFER_SIZE"},
        {AUDCLNT_E_BUFFER_SIZE_ERROR, "AUDCLNT_E_BUFFER_SIZE_ERROR"},
        {AUDCLNT_E_CPUUSAGE_EXCEEDED, "AUDCLNT_E_CPUUSAGE_EXCEEDED"},
        {AUDCLNT_E_BUFFER_ERROR, "AUDCLNT_E_BUFFER_ERROR"},
        {AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED, "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED"},
        {AUDCLNT_E_INVALID_DEVICE_PERIOD, "AUDCLNT_E_INVALID_DEVICE_PERIOD"},
        {AUDCLNT_E_INVALID_STREAM_FLAG, "AUDCLNT_E_INVALID_STREAM_FLAG"},
        {AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE, "AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE"},
        {AUDCLNT_E_RESOURCES_INVALIDATED, "AUDCLNT_E_RESOURCES_INVALIDATED"},
    };
    for (const auto& entry : names) {
        if (entry.code == result) return entry.name;
    }
    return "HRESULT";
}

std::vector<WasapiEndpointRecord> enumerate_wasapi_endpoints()
{
    ComApartment apartment;
    std::vector<WasapiEndpointRecord> records;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator)))) {
        return records;
    }

    std::wstring default_id;
    IMMDevice* default_device = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &default_device)) &&
        default_device != nullptr) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(default_device->GetId(&id)) && id != nullptr) {
            default_id = id;
            CoTaskMemFree(id);
        }
        default_device->Release();
    }

    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) &&
        collection != nullptr) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT index = 0; index < count; ++index) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(index, &device)) || device == nullptr) continue;
            WasapiEndpointRecord record;
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
                record.id = id;
                CoTaskMemFree(id);
            }
            IPropertyStore* properties = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
                properties != nullptr) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
                    record.wide_name = value.pwszVal;
                }
                PropVariantClear(&value);
                properties->Release();
            }
            record.name = narrow_string(record.wide_name, CP_UTF8);
            record.is_default = !record.id.empty() && record.id == default_id;
            records.push_back(std::move(record));
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    return records;
}

WasapiSession::WasapiSession()
{
    com_initialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
}

WasapiSession::~WasapiSession()
{
    close();
    if (com_initialized_) CoUninitialize();
}

bool WasapiSession::open(const std::string& endpoint_name, std::string* error_message)
{
    close();
    const auto endpoints = enumerate_wasapi_endpoints();
    if (endpoints.empty()) {
        if (error_message) *error_message = "No active WASAPI render endpoint was found";
        return false;
    }

    const WasapiEndpointRecord* match = nullptr;
    if (endpoint_name.empty()) {
        match = &endpoints.front();
    } else {
        const auto needle = lowercase_ascii(endpoint_name);
        for (const auto& candidate : endpoints) {
            if (narrow_string(candidate.id, CP_UTF8) == endpoint_name) {
                match = &candidate;
                break;
            }
            if (lowercase_ascii(candidate.name).find(needle) != std::string::npos) {
                match = &candidate;
                break;
            }
        }
    }
    if (match == nullptr) {
        if (error_message) {
            *error_message = "No active WASAPI render endpoint matches \"" + endpoint_name + "\"";
        }
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
        if (error_message) {
            *error_message = std::string("MMDeviceEnumerator creation failed: ") +
                             wasapi_error_name(hr);
        }
        return false;
    }
    hr = enumerator->GetDevice(match->id.c_str(), &device_);
    enumerator->Release();
    if (FAILED(hr) || device_ == nullptr) {
        if (error_message) {
            *error_message = std::string("IMMDeviceEnumerator::GetDevice failed: ") +
                             wasapi_error_name(hr);
        }
        close();
        return false;
    }
    endpoint_ = *match;
    if (!activate_client(error_message)) {
        close();
        return false;
    }
    return true;
}

bool WasapiSession::activate_client(std::string* error_message)
{
    if (device_ == nullptr) {
        if (error_message) *error_message = "No WASAPI endpoint is open";
        return false;
    }
    safe_release(&render_client_);
    safe_release(&client_);
    const HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(&client_));
    if (FAILED(hr) || client_ == nullptr) {
        if (error_message) {
            *error_message = std::string("IMMDevice::Activate(IAudioClient) failed: ") +
                             wasapi_error_name(hr);
        }
        return false;
    }
    return true;
}

void WasapiSession::release_client()
{
    safe_release(&render_client_);
    safe_release(&client_);
    if (event_ != nullptr) {
        CloseHandle(event_);
        event_ = nullptr;
    }
    buffer_frame_count_ = 0;
    stream_period_ms_ = 0.0;
    started_ = false;
}

void WasapiSession::close()
{
    if (client_ != nullptr && started_) {
        client_->Stop();
        client_->Reset();
    }
    release_client();
    safe_release(&device_);
    endpoint_ = WasapiEndpointRecord{};
    format_ = WasapiNegotiatedFormat{};
}

HRESULT WasapiSession::is_format_supported(const WAVEFORMATEXTENSIBLE& format) const
{
    if (client_ == nullptr) return AUDCLNT_E_NOT_INITIALIZED;
    return client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr);
}

bool WasapiSession::negotiate_format(const WasapiFormatRequest& request,
                                     WasapiNegotiatedFormat* result,
                                     std::string* error_message) const
{
    if (client_ == nullptr) {
        if (error_message) *error_message = "No WASAPI endpoint is open";
        return false;
    }
    if (result == nullptr || request.sample_rate <= 0.0 || request.channels == 0) {
        if (error_message) *error_message = "Invalid WASAPI format request";
        return false;
    }

    // Every mode negotiates as an ordinary PCM-tagged WAVEFORMATEXTENSIBLE.
    // DoP is literally 24-bit PCM carrying the 0x05/0xfa marker, and Native DSD
    // reaches alt-setting 4 through the same 4-byte subslot that alt-setting 3
    // uses, so the container width plus the sample rate are the only knobs the
    // WASAPI object model exposes for the alt 3/alt 4 choice.
    std::string tried;
    for (const auto& candidate : bit_candidates(request.mode, request.preferred_container_bits)) {
        const auto format = make_extensible(request.sample_rate, request.channels,
                                            candidate.container, candidate.valid,
                                            kSubformatPcm);
        const HRESULT hr = is_format_supported(format);
        if (!tried.empty()) tried += ", ";
        tried += std::to_string(candidate.container) + "/" + std::to_string(candidate.valid) +
                 "=" + wasapi_error_name(hr);
        if (hr == S_OK) {
            result->format = format;
            result->container_bits = candidate.container;
            result->valid_bits = candidate.valid;
            result->channels = request.channels;
            result->sample_rate = request.sample_rate;
            result->description = describe_waveformat(&format.Format);
            result->valid = true;
            return true;
        }
    }
    if (error_message) {
        *error_message = std::string("No exclusive-mode format for ") +
                         wasapi_output_mode_name(request.mode) + " at " +
                         std::to_string(static_cast<long long>(request.sample_rate + 0.5)) +
                         " Hz, " + std::to_string(request.channels) + " ch (tried " + tried + ")";
    }
    return false;
}

bool WasapiSession::initialize_exclusive(const WasapiNegotiatedFormat& format,
                                         std::string* error_message)
{
    if (client_ == nullptr) {
        if (error_message) *error_message = "No WASAPI endpoint is open";
        return false;
    }
    if (!format.valid) {
        if (error_message) *error_message = "The WASAPI format was not negotiated";
        return false;
    }

    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME minimum_period = 0;
    HRESULT hr = client_->GetDevicePeriod(&default_period, &minimum_period);
    if (FAILED(hr)) {
        if (error_message) {
            *error_message = std::string("IAudioClient::GetDevicePeriod failed: ") +
                             wasapi_error_name(hr);
        }
        return false;
    }
    REFERENCE_TIME period = default_period > 0 ? default_period : minimum_period;

    hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             period, period, &format.format.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Documented retry: ask for the aligned frame count, convert it back to
        // a period, and start over on a fresh IAudioClient.
        UINT32 aligned_frames = 0;
        if (SUCCEEDED(client_->GetBufferSize(&aligned_frames)) && aligned_frames > 0) {
            period = static_cast<REFERENCE_TIME>(
                10000.0 * 1000.0 / format.sample_rate * aligned_frames + 0.5);
            if (!activate_client(error_message)) return false;
            hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                     &format.format.Format, nullptr);
        }
    }
    if (FAILED(hr)) {
        if (error_message) {
            *error_message = std::string("IAudioClient::Initialize (exclusive) failed: ") +
                             wasapi_error_name(hr) + " for " +
                             describe_waveformat(&format.format.Format);
        }
        return false;
    }

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_ == nullptr) {
        if (error_message) *error_message = "CreateEvent for the render callback failed";
        return false;
    }
    hr = client_->SetEventHandle(event_);
    if (FAILED(hr)) {
        if (error_message) {
            *error_message = std::string("IAudioClient::SetEventHandle failed: ") +
                             wasapi_error_name(hr);
        }
        return false;
    }
    hr = client_->GetBufferSize(&buffer_frame_count_);
    if (FAILED(hr) || buffer_frame_count_ == 0) {
        if (error_message) {
            *error_message = std::string("IAudioClient::GetBufferSize failed: ") +
                             wasapi_error_name(hr);
        }
        return false;
    }
    hr = client_->GetService(__uuidof(IAudioRenderClient),
                             reinterpret_cast<void**>(&render_client_));
    if (FAILED(hr) || render_client_ == nullptr) {
        if (error_message) {
            *error_message = std::string("IAudioClient::GetService(IAudioRenderClient) failed: ") +
                             wasapi_error_name(hr);
        }
        return false;
    }
    format_ = format;
    stream_period_ms_ = 1000.0 * buffer_frame_count_ / format.sample_rate;
    return true;
}

bool WasapiSession::start(std::string* error_message)
{
    if (client_ == nullptr || render_client_ == nullptr) {
        if (error_message) *error_message = "The WASAPI render stream is not initialized";
        return false;
    }
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        if (error_message) {
            *error_message = std::string("IAudioClient::Start failed: ") + wasapi_error_name(hr);
        }
        return false;
    }
    started_ = true;
    return true;
}

void WasapiSession::stop()
{
    if (client_ != nullptr && started_) {
        client_->Stop();
        client_->Reset();
    }
    started_ = false;
}

} // namespace wasio
