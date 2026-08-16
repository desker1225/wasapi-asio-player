#include "player/engine_factory.h"

#include "asio/driver_registry.h"
#include "formats/dff_reader.h"
#include "formats/dsf_reader.h"
#include "formats/wav_reader.h"
#include "playback/pcm_playback.h"
#include "wasapi/wasapi_playback.h"
#include "wasapi/wasapi_session.h"

#include <algorithm>
#include <cctype>

namespace wasio {
namespace {

// DoP carries 16 DSD bits per PCM frame, so DSD512 would need a 1,411,200 Hz
// carrier. Refused rather than silently downgraded.
constexpr std::uint64_t kMaxDopDsdRate = 11289600;

std::string lowercase_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string file_extension(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    const std::size_t separator = path.find_last_of("/\\");
    if (separator != std::string::npos && dot < separator) return {};
    return lowercase_ascii(path.substr(dot + 1));
}

std::unique_ptr<AudioSource> open_pcm_source(const std::string& path, std::string* error_message)
{
    auto source = std::make_unique<WavPcmSource>(path);
    if (!source->valid()) {
        if (error_message) *error_message = source->error();
        return nullptr;
    }
    return source;
}

std::unique_ptr<DsdSource> open_dsd_source(const std::string& path, std::string* error_message)
{
    if (file_extension(path) == "dff") {
        auto source = std::make_unique<DffDsdSource>(path);
        if (!source->valid()) {
            if (error_message) *error_message = source->error();
            return nullptr;
        }
        return source;
    }
    auto source = std::make_unique<DsfDsdSource>(path);
    if (!source->valid()) {
        if (error_message) *error_message = source->error();
        return nullptr;
    }
    return source;
}

// ---- adapters ----

class AsioPcmEngine final : public IPlaybackEngine {
public:
    explicit AsioPcmEngine(PcmPlaybackConfig config) : config_(std::move(config)) {}

    bool start(std::string* error_message) override
    {
        return playback_.start(std::move(config_), error_message);
    }
    EngineStartFailure start_failure() const override { return playback_.start_failure(); }
    void stop() override { playback_.stop(); }
    bool is_running() const override { return playback_.is_running(); }
    bool playback_finished() const override { return playback_.playback_finished(); }
    void set_paused(bool paused) override { playback_.set_paused(paused); }
    bool is_paused() const override { return playback_.is_paused(); }
    bool request_seek(std::uint64_t source_frame) override
    {
        return playback_.request_seek(source_frame);
    }
    bool seek_in_progress() const override { return playback_.seek_in_progress(); }
    std::uint64_t output_position_frames() const override
    {
        return playback_.output_position_frames();
    }
    std::uint64_t total_output_frames() const override { return playback_.total_output_frames(); }
    double output_frame_rate() const override { return playback_.output_frame_rate(); }
    std::size_t source_frames_per_output() const override
    {
        return playback_.source_frames_per_output();
    }
    std::uint64_t underrun_count() const override { return playback_.underrun_count(); }
    std::uint64_t callback_count() const override { return playback_.buffer_switch_count(); }
    std::string format_description() const override
    {
        const auto& format = playback_.output_format();
        const char* bits = format.encoding == PcmEncoding::Int16
                               ? "16-bit"
                               : (format.encoding == PcmEncoding::Int24 ? "24-bit" : "32-bit");
        return "ASIO PCM " + std::to_string(static_cast<long long>(format.sample_rate)) + " Hz, " +
               std::to_string(format.channels) + " ch, " + bits + ", buffer " +
               std::to_string(playback_.buffer_size());
    }
    std::string error_message() const override { return {}; }

private:
    PcmPlaybackConfig config_;
    PcmPlayback playback_;
};

class AsioDsdEngine final : public IPlaybackEngine {
public:
    explicit AsioDsdEngine(DsdPlaybackConfig config) : config_(std::move(config)) {}

    bool start(std::string* error_message) override
    {
        return playback_.start(std::move(config_), error_message);
    }
    EngineStartFailure start_failure() const override { return playback_.start_failure(); }
    void stop() override { playback_.stop(); }
    bool is_running() const override { return playback_.is_running(); }
    bool playback_finished() const override { return playback_.playback_finished(); }
    void set_paused(bool paused) override { playback_.set_paused(paused); }
    bool is_paused() const override { return playback_.is_paused(); }
    bool request_seek(std::uint64_t source_frame) override
    {
        return playback_.request_seek(source_frame);
    }
    bool seek_in_progress() const override { return playback_.seek_in_progress(); }
    std::uint64_t output_position_frames() const override
    {
        return playback_.output_position_frames();
    }
    std::uint64_t total_output_frames() const override { return playback_.total_output_frames(); }
    double output_frame_rate() const override { return playback_.output_frame_rate(); }
    std::size_t source_frames_per_output() const override
    {
        return playback_.source_frames_per_output();
    }
    std::uint64_t underrun_count() const override { return playback_.underrun_count(); }
    std::uint64_t callback_count() const override { return playback_.buffer_switch_count(); }
    std::string format_description() const override
    {
        const std::string mode = playback_.mode() == DsdOutputMode::Native ? "Native DSD" : "DoP";
        return "ASIO " + mode + " | DSD " +
               std::to_string(static_cast<long long>(playback_.dsd_sample_rate())) + " Hz | host " +
               std::to_string(static_cast<long long>(playback_.output_sample_rate())) + " Hz";
    }
    std::string error_message() const override { return {}; }

private:
    DsdPlaybackConfig config_;
    DsdPlayback playback_;
};

class WasapiEngine final : public IPlaybackEngine {
public:
    explicit WasapiEngine(WasapiPlaybackConfig config) : config_(std::move(config)) {}

    bool start(std::string* error_message) override
    {
        return playback_.start(std::move(config_), error_message);
    }
    EngineStartFailure start_failure() const override { return playback_.start_failure(); }
    void stop() override { playback_.stop(); }
    bool is_running() const override { return playback_.is_running(); }
    bool playback_finished() const override { return playback_.playback_finished(); }
    void set_paused(bool paused) override { playback_.set_paused(paused); }
    bool is_paused() const override { return playback_.is_paused(); }
    bool request_seek(std::uint64_t source_frame) override
    {
        return playback_.request_seek(source_frame);
    }
    bool seek_in_progress() const override { return playback_.seek_in_progress(); }
    std::uint64_t output_position_frames() const override
    {
        return playback_.output_position_frames();
    }
    std::uint64_t total_output_frames() const override { return playback_.total_output_frames(); }
    double output_frame_rate() const override { return playback_.output_frame_rate(); }
    std::size_t source_frames_per_output() const override
    {
        return playback_.source_frames_per_output();
    }
    std::uint64_t underrun_count() const override { return playback_.underrun_count(); }
    std::uint64_t callback_count() const override { return playback_.buffer_switch_count(); }
    std::string format_description() const override
    {
        return std::string("WASAPI exclusive ") + wasapi_output_mode_name(playback_.mode()) + " | " +
               playback_.format_description();
    }
    std::string error_message() const override { return playback_.render_error(); }

private:
    WasapiPlaybackConfig config_;
    WasapiPlayback playback_;
};

} // namespace

const char* playback_backend_name(PlaybackBackend backend)
{
    switch (backend) {
    case PlaybackBackend::Asio: return "ASIO";
    case PlaybackBackend::Wasapi: return "WASAPI";
    }
    return "unknown";
}

bool resolve_device(PlaybackBackend backend, std::string* device)
{
    if (device == nullptr) return false;
    if (backend == PlaybackBackend::Asio) {
        for (const auto& driver : enumerate_registered_drivers()) {
            const std::string display =
                driver.description.empty() ? driver.registry_name : driver.description;
            const bool matches = device->empty()
                                     ? (driver.is_x64 && driver.dll_exists)
                                     : (display == *device || driver.registry_name == *device);
            if (matches) {
                *device = display;
                return true;
            }
        }
        return false;
    }
    const auto endpoints = enumerate_wasapi_endpoints();
    if (endpoints.empty()) return false;
    if (device->empty()) return true;
    for (const auto& endpoint : endpoints) {
        if (narrow_string(endpoint.id, CP_UTF8) == *device) return true;
        if (endpoint.name.find(*device) != std::string::npos) return true;
    }
    return false;
}

std::unique_ptr<IPlaybackEngine> create_engine(const EngineRequest& request,
                                               std::string* error_message)
{
    const TrackInfo& track = request.track;
    if (!track.valid) {
        if (error_message) {
            *error_message = track.error.empty() ? "the track could not be read" : track.error;
        }
        return nullptr;
    }
    if (track.kind == TrackKind::DsdFile && request.dsd_mode == DsdOutputMode::DoP &&
        track.sample_rate > static_cast<double>(kMaxDopDsdRate)) {
        if (error_message) {
            *error_message = "DoP512 is not supported: it requires a 1,411,200 Hz PCM host rate";
        }
        return nullptr;
    }

    if (request.backend == PlaybackBackend::Asio) {
        if (track.kind == TrackKind::PcmFile) {
            auto source = open_pcm_source(track.path, error_message);
            if (!source) return nullptr;
            PcmPlaybackConfig config;
            config.driver_name = request.device;
            config.source = std::move(source);
            return std::make_unique<AsioPcmEngine>(std::move(config));
        }
        auto source = open_dsd_source(track.path, error_message);
        if (!source) return nullptr;
        DsdPlaybackConfig config;
        config.driver_name = request.device;
        config.source = std::move(source);
        config.mode = request.dsd_mode;
        return std::make_unique<AsioDsdEngine>(std::move(config));
    }

    WasapiPlaybackConfig config;
    config.endpoint_name = request.device;
    if (track.kind == TrackKind::PcmFile) {
        auto source = open_pcm_source(track.path, error_message);
        if (!source) return nullptr;
        config.mode = WasapiOutputMode::Pcm;
        config.pcm_source = std::move(source);
    } else {
        if (request.dsd_mode == DsdOutputMode::Native) {
            if (error_message) {
                *error_message = "WASAPI does not support Native DSD: use DoP or switch to ASIO";
            }
            return nullptr;
        }
        auto source = open_dsd_source(track.path, error_message);
        if (!source) return nullptr;
        config.mode = WasapiOutputMode::DoP;
        config.dsd_source = std::move(source);
    }
    return std::make_unique<WasapiEngine>(std::move(config));
}

} // namespace wasio
