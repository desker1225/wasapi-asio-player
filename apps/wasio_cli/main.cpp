#include "asio/driver_registry.h"
#include "formats/utf8_file.h"
#include "wasapi/wasapi_session.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <shellapi.h>

#include <iostream>
#include <string>
#include <vector>

int run_play_command(int argc, char** argv);
#ifdef WASIO_WITH_PROBE
int run_probe_command(int argc, char** argv);
#endif

namespace {

void print_usage()
{
    std::cout << "Usage:\n"
              << "  WasioPlay --list-devices\n"
#ifdef WASIO_WITH_PROBE
              << "  WasioPlay --probe [--backend asio|wasapi] [--device <name>]\n"
#endif
              << "  WasioPlay [--backend asio|wasapi] [--device <name>] [--dsd native|dop]\n"
                 "            [--repeat off|one|all] [--shuffle] <file>...\n"
              << "  WasioPlay [--backend ...] --playlist <m3u8> [--repeat off|one|all] "
                 "[--shuffle]\n"
              << "\n"
              << "During playback: space pause/resume, left/right seek -/+10 s,\n"
              << "n/p next/previous track, q quit.\n";
}

int list_devices()
{
    const auto drivers = wasio::enumerate_registered_drivers();
    std::cout << "ASIO drivers (x64 registered):\n";
    if (drivers.empty()) {
        std::cout << "  (none found in HKLM\\SOFTWARE\\ASIO)\n";
    }
    for (const auto& driver : drivers) {
        std::cout << "  - " << (driver.description.empty() ? driver.registry_name : driver.description)
                  << " | " << driver.architecture << " | "
                  << (driver.dll_exists ? driver.dll_path : "DLL missing") << "\n";
    }

    const auto endpoints = wasio::enumerate_wasapi_endpoints();
    std::cout << "WASAPI render endpoints:\n";
    if (endpoints.empty()) {
        std::cout << "  (none active)\n";
    }
    for (const auto& endpoint : endpoints) {
        std::cout << "  - " << endpoint.name << (endpoint.is_default ? " [default]" : "") << "\n";
    }
    return 0;
}

} // namespace

int main()
{
    // Windows hands main() its arguments in the ANSI code page, but paths are
    // UTF-8 everywhere inside this project (TrackInfo::path, M3U8 files), and a
    // non-ASCII file name cannot survive a round trip through ANSI on a machine
    // whose code page cannot represent it. Take the wide command line instead
    // and convert once, here at the boundary.
    int wide_count = 0;
    LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    std::vector<std::string> utf8_arguments;
    std::vector<char*> argv_storage;
    if (wide_argv != nullptr) {
        utf8_arguments.reserve(static_cast<std::size_t>(wide_count));
        for (int i = 0; i < wide_count; ++i) {
            utf8_arguments.push_back(wasio::narrow_to_utf8(wide_argv[i]));
        }
        LocalFree(wide_argv);
    }
    argv_storage.reserve(utf8_arguments.size() + 1);
    for (auto& argument : utf8_arguments) argv_storage.push_back(argument.data());
    argv_storage.push_back(nullptr);
    const int argc = static_cast<int>(utf8_arguments.size());
    char** argv = argv_storage.data();

    // Console output is UTF-8 too, so track names print as themselves.
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string command = argv[1];
    if (command == "--list-devices") {
        return list_devices();
    }
#ifdef WASIO_WITH_PROBE
    if (command == "--probe") {
        return run_probe_command(argc, argv);
    }
#endif
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }
    // Anything else is a playback invocation: options plus a file path.
    return run_play_command(argc, argv);
}
