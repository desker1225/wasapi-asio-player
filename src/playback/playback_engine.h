#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wasio {

// Why start() failed, so the CLI can tell "this device will not open" from
// "this device will not take this format" (exit codes 3 and 5 in README.md).
enum class EngineStartFailure {
    None,
    // The device could not be opened, initialized or started at all. This is
    // where AUDCLNT_E_DEVICE_IN_USE lands.
    DeviceOpen,
    // The device opened but refused the sample rate, container or channel
    // layout the track needs.
    FormatNegotiation,
};

// What PlayerController needs from a running output, independent of backend.
// PcmPlayback/DsdPlayback/WasapiPlayback each take a different config type in
// start(), so the adapters in player/engine_factory.h hold the config and
// implement this; the realtime classes themselves stay free of a vtable.
// A fake implementation is what lets PlayerController be unit tested without
// hardware.
class IPlaybackEngine {
public:
    virtual ~IPlaybackEngine() = default;

    virtual bool start(std::string* error_message) = 0;
    // Only meaningful after start() returned false.
    virtual EngineStartFailure start_failure() const = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
    virtual bool playback_finished() const = 0;

    virtual void set_paused(bool paused) = 0;
    virtual bool is_paused() const = 0;
    virtual bool request_seek(std::uint64_t source_frame) = 0;
    virtual bool seek_in_progress() const = 0;

    virtual std::uint64_t output_position_frames() const = 0;
    virtual std::uint64_t total_output_frames() const = 0;
    // Frames per second of output_position_frames(); see the comment on
    // DsdPlayback::output_frame_rate() for why this is not the device sample
    // rate in ASIO Native DSD mode.
    virtual double output_frame_rate() const = 0;
    virtual std::size_t source_frames_per_output() const = 0;

    virtual std::uint64_t underrun_count() const = 0;
    virtual std::uint64_t callback_count() const = 0;
    virtual std::string format_description() const = 0;
    // Non-empty when the output stopped on its own; only meaningful after stop().
    virtual std::string error_message() const = 0;
};

} // namespace wasio
