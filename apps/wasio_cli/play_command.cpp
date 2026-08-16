#include "player/player_controller.h"
#include "player/track_info.h"

#include <conio.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Exit codes; the table in README.md is the reference.
constexpr int kExitOk = 0;
constexpr int kExitBadArguments = 1;
constexpr int kExitDeviceNotFound = 2;
constexpr int kExitDeviceOpenFailed = 3;
constexpr int kExitFileFailed = 4;
constexpr int kExitFormatNegotiationFailed = 5;
constexpr int kExitDoP512Rejected = 6;
constexpr int kExitPlaybackInterrupted = 7;

constexpr const char* kProbeHint = "Run --probe to check what this device supports.";

struct PlayOptions {
    wasio::PlaybackBackend backend = wasio::PlaybackBackend::Asio;
    std::string device;
    wasio::DsdOutputMode dsd_mode = wasio::DsdOutputMode::Native;
    wasio::RepeatMode repeat = wasio::RepeatMode::Off;
    bool shuffle = false;
    std::string playlist_path;
    std::vector<std::string> files;
};

std::string format_clock(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    const auto total = static_cast<long long>(seconds + 0.5);
    char text[32];
    std::snprintf(text, sizeof(text), "%02lld:%02lld", total / 60, total % 60);
    return text;
}

void print_play_usage()
{
    std::cout << "Usage:\n"
              << "  WasioPlay [--backend asio|wasapi] [--device <name>] [--dsd native|dop]\n"
              << "            [--repeat off|one|all] [--shuffle] <file>...\n"
              << "  WasioPlay [--backend ...] --playlist <m3u8> [--repeat off|one|all] "
                 "[--shuffle]\n"
              << "\n"
              << "  space  pause / resume      left/right  seek -/+ 10 s\n"
              << "  n      next track          p           previous track\n"
              << "  q      quit\n";
}

// Exit codes, taken from the controller's classification rather than from
// the message text.
int exit_code_for(wasio::PlayerErrorKind kind)
{
    switch (kind) {
    case wasio::PlayerErrorKind::None: return kExitOk;
    case wasio::PlayerErrorKind::DeviceNotFound: return kExitDeviceNotFound;
    case wasio::PlayerErrorKind::DeviceOpenFailed: return kExitDeviceOpenFailed;
    case wasio::PlayerErrorKind::FileFailed: return kExitFileFailed;
    case wasio::PlayerErrorKind::FormatNegotiationFailed: return kExitFormatNegotiationFailed;
    case wasio::PlayerErrorKind::DoP512Rejected: return kExitDoP512Rejected;
    case wasio::PlayerErrorKind::PlaybackInterrupted: return kExitPlaybackInterrupted;
    }
    return kExitDeviceOpenFailed;
}

} // namespace

int run_play_command(int argc, char** argv)
{
    PlayOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--backend" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "asio") {
                options.backend = wasio::PlaybackBackend::Asio;
            } else if (value == "wasapi") {
                options.backend = wasio::PlaybackBackend::Wasapi;
            } else {
                std::cerr << "Unknown backend: " << value << " (expected asio|wasapi)\n";
                return kExitBadArguments;
            }
        } else if (argument == "--device" && i + 1 < argc) {
            options.device = argv[++i];
        } else if (argument == "--dsd" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "native") {
                options.dsd_mode = wasio::DsdOutputMode::Native;
            } else if (value == "dop") {
                options.dsd_mode = wasio::DsdOutputMode::DoP;
            } else {
                std::cerr << "Unknown DSD mode: " << value << " (expected native|dop)\n";
                return kExitBadArguments;
            }
        } else if (argument == "--repeat" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (!wasio::parse_repeat_mode(value, &options.repeat)) {
                std::cerr << "Unknown repeat mode: " << value << " (expected off|one|all)\n";
                return kExitBadArguments;
            }
        } else if (argument == "--shuffle") {
            options.shuffle = true;
        } else if (argument == "--playlist" && i + 1 < argc) {
            options.playlist_path = argv[++i];
        } else if (!argument.empty() && argument[0] == '-') {
            std::cerr << "Unknown argument: " << argument << "\n";
            print_play_usage();
            return kExitBadArguments;
        } else {
            options.files.push_back(argument);
        }
    }
    if (options.playlist_path.empty() && options.files.empty()) {
        print_play_usage();
        return kExitBadArguments;
    }
    if (!options.playlist_path.empty() && !options.files.empty()) {
        std::cerr << "Use either --playlist or a list of files, not both.\n";
        return kExitBadArguments;
    }

    wasio::PlayerController controller;
    controller.set_backend(options.backend);
    controller.set_device(options.device);
    controller.set_dsd_mode(options.dsd_mode);

    auto& playlist = controller.playlist();
    if (!options.playlist_path.empty()) {
        std::string error;
        if (!playlist.load_m3u8(options.playlist_path, &error)) {
            std::cerr << "Cannot read playlist " << options.playlist_path << ": " << error << "\n";
            return kExitFileFailed;
        }
        if (playlist.empty()) {
            std::cerr << "Playlist " << options.playlist_path << " has no entries.\n";
            return kExitFileFailed;
        }
    } else {
        for (const auto& path : options.files) playlist.add(wasio::probe_track(path));
    }
    // Repeat/shuffle are set after loading so shuffle starts its round from
    // the first track rather than an empty list.
    playlist.set_repeat(options.repeat);
    playlist.set_shuffle(options.shuffle);

    std::size_t unusable = 0;
    for (const auto& track : playlist.tracks()) {
        if (track.valid) continue;
        ++unusable;
        std::cerr << "Skipping " << track.display_name << ": " << track.error << "\n";
    }
    if (unusable == playlist.size()) {
        std::cerr << "No playable track in the playlist.\n";
        return kExitFileFailed;
    }

    std::cout << playlist.size() << " track(s) | repeat " << wasio::repeat_mode_name(options.repeat)
              << (options.shuffle ? " | shuffle" : "") << "\n";
    for (std::size_t i = 0; i < playlist.size(); ++i) {
        const auto& track = playlist.at(i);
        std::cout << "  " << (i + 1) << ". " << track.display_name;
        if (track.valid) {
            std::cout << " | " << wasio::track_kind_name(track.kind) << " | "
                      << static_cast<long long>(track.sample_rate) << " Hz | " << track.channels
                      << " ch | " << format_clock(track.duration_seconds);
        } else {
            std::cout << " | unavailable";
        }
        std::cout << "\n";
    }

    // first_index() rather than 0: under shuffle the round starts wherever the
    // permutation begins, and entering at 0 would cut the round short.
    const std::size_t start_index = playlist.current_index() == wasio::Playlist::kInvalidIndex
                                        ? playlist.first_index()
                                        : playlist.current_index();
    if (start_index == wasio::Playlist::kInvalidIndex || !controller.play(start_index)) {
        const auto status = controller.status();
        std::cerr << "Playback could not start: " << status.error << "\n" << kProbeHint << "\n";
        return exit_code_for(status.error_kind);
    }
    std::cout << "space pause/resume | left/right seek -/+10 s | n/p track | q quit\n";

    bool quit = false;
    std::string last_format;
    while (!quit) {
        const auto status = controller.poll();
        if (status.state == wasio::PlayerState::Stopped) break;

        while (_kbhit()) {
            const int key = _getch();
            if (key == 'q' || key == 'Q') { quit = true; break; }
            if (key == ' ') {
                if (status.state == wasio::PlayerState::Paused) controller.resume();
                else controller.pause();
            } else if (key == 'n' || key == 'N') {
                if (!controller.next()) quit = true;
            } else if (key == 'p' || key == 'P') {
                controller.previous();
            } else if (key == 0 || key == 0xe0) {
                // Arrow keys arrive as a two-byte sequence.
                const int arrow = _getch();
                if (arrow == 75) controller.seek(status.position_seconds - 10.0);
                else if (arrow == 77) controller.seek(status.position_seconds + 10.0);
            }
        }
        if (quit) break;

        // Reprint the header whenever a track change renegotiates the format,
        // so a cross-sample-rate change is visible.
        if (status.format_description != last_format) {
            last_format = status.format_description;
            std::cout << "\n" << (status.track_index + 1) << ". " << status.track_name << " | "
                      << last_format << "\n";
        }
        std::cout << "\r  [" << wasio::player_state_name(status.state) << "] "
                  << format_clock(status.position_seconds) << " / "
                  << format_clock(status.duration_seconds)
                  << "  callbacks=" << status.callback_count
                  << " underruns=" << status.underrun_count << "    " << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cout << "\n";

    // stop() first: a render-thread failure is only readable once the thread
    // has been joined, and stop() is what joins it.
    controller.stop();
    const auto final_status = controller.status();
    if (!final_status.error.empty()) {
        std::cerr << "Playback stopped early: " << final_status.error << "\n" << kProbeHint << "\n";
        return exit_code_for(final_status.error_kind);
    }
    std::cout << (quit ? "Stopped" : "Finished") << " | callbacks=" << final_status.callback_count
              << " underruns=" << final_status.underrun_count << "\n";
    return kExitOk;
}
