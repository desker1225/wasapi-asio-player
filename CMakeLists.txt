cmake_minimum_required(VERSION 3.21)

project(WasioPlayer VERSION 1.0.1 LANGUAGES CXX)

include(CTest)

# The single source of the product name and version. src/version.h is generated
# from these, and the resource script reads the generated header, so a release
# is bumped by editing project(... VERSION ...) above and nothing else.
set(WASIO_APP_NAME "wasio-player")
set(WASIO_APP_DESCRIPTION "Bit-Perfect Audio Player (WASAPI & ASIO)")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
               "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)

if(NOT WIN32)
    message(FATAL_ERROR "wasio-player currently requires Windows")
endif()

# Link the CRT statically so the executable runs on a clean Windows install
# with no Visual C++ Redistributable to fetch first. A dynamically linked
# Debug build is worse than merely inconvenient: MSVCP140D.dll ships only with
# Visual Studio, so such a binary cannot start anywhere else at all.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# CP1b: ASIO SDK integration joins the build. The SDK itself stays outside
# this repository (Steinberg license); ASIO_SDK_ROOT points at a local
# ASIOSDK234 checkout.
set(ASIO_SDK_ROOT "" CACHE PATH "Path to the external Steinberg ASIO SDK 2.3.x")
if(NOT ASIO_SDK_ROOT)
    message(FATAL_ERROR "Set ASIO_SDK_ROOT to the external ASIOSDK234 directory")
endif()

# Internal-only: pulls in asio_probe/wasapi_probe and the CLI's --probe
# command. The public release tree does not carry those files, so it leaves
# this option off.
option(WASIO_WITH_PROBE "Build the ASIO/WASAPI device capability probe" OFF)

set(ASIO_COMMON_DIR "${ASIO_SDK_ROOT}/common")
set(ASIO_HOST_DIR "${ASIO_SDK_ROOT}/host")
set(ASIO_PC_DIR "${ASIO_HOST_DIR}/pc")

foreach(required_file
        "${ASIO_COMMON_DIR}/asio.cpp"
        "${ASIO_COMMON_DIR}/asio.h"
        "${ASIO_HOST_DIR}/asiodrivers.cpp"
        "${ASIO_PC_DIR}/asiolist.cpp")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "ASIO SDK file not found: ${required_file}")
    endif()
endforeach()

add_library(asio_sdk_host STATIC
    "${ASIO_COMMON_DIR}/asio.cpp"
    "${ASIO_HOST_DIR}/asiodrivers.cpp"
    "${ASIO_PC_DIR}/asiolist.cpp")

target_include_directories(asio_sdk_host PUBLIC
    "${ASIO_COMMON_DIR}"
    "${ASIO_HOST_DIR}"
    "${ASIO_PC_DIR}")
target_compile_features(asio_sdk_host PUBLIC cxx_std_17)
target_compile_options(asio_sdk_host PRIVATE /W3 /EHsc)
target_link_libraries(asio_sdk_host PUBLIC ole32 advapi32 user32)

add_library(wasio_core STATIC
    src/audio/dsd_packer.cpp
    src/formats/wav_reader.cpp
    src/formats/dff_reader.cpp
    src/formats/dsf_reader.cpp
    src/formats/utf8_file.cpp
    src/playback/ring_buffer.cpp
    src/playback/pcm_playback.cpp
    src/playback/dsd_playback.cpp
    src/player/track_info.cpp
    src/player/playlist.cpp
    src/player/engine_factory.cpp
    src/player/player_controller.cpp
    src/asio/asio_session.cpp
    src/asio/driver_registry.cpp
    src/wasapi/wasapi_session.cpp
    src/wasapi/wasapi_playback.cpp)
if(WASIO_WITH_PROBE)
    target_sources(wasio_core PRIVATE
        src/asio/asio_probe.cpp
        src/wasapi/wasapi_probe.cpp)
endif()
target_include_directories(wasio_core PUBLIC src "${CMAKE_BINARY_DIR}/generated")
# ole32 comes in through asio_sdk_host and covers the MMDevice/IAudioClient
# COM activation; avrt is only needed for the MMCSS boost on the render thread.
# shell32 is for CommandLineToArgvW, which the CLI uses to get UTF-8 arguments.
target_link_libraries(wasio_core PUBLIC asio_sdk_host avrt shell32)
target_compile_features(wasio_core PUBLIC cxx_std_17)
target_compile_options(wasio_core PRIVATE /W4 /permissive- /EHsc /utf-8)

# The resource script includes the generated version.h, but the RC compiler's
# includes are not scanned for dependencies, so without this the .res keeps a
# stale version and the file properties disagree with the running program.
set_source_files_properties(apps/wasio_player/wasio_player.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/generated/version.h")

add_executable(WasioPlayer WIN32
    apps/wasio_player/main.cpp
    apps/wasio_player/wasio_player.rc
    src/icons/wasio_player.ico)
target_link_libraries(WasioPlayer PRIVATE wasio_core comctl32 comdlg32)
target_compile_features(WasioPlayer PRIVATE cxx_std_17)
target_compile_options(WasioPlayer PRIVATE /W4 /permissive- /EHsc /utf-8)
set_target_properties(WasioPlayer PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(BUILD_TESTING)
    add_executable(player_unit tests/player_unit.cpp)
    target_link_libraries(player_unit PRIVATE wasio_core)
    target_compile_features(player_unit PRIVATE cxx_std_17)
    target_compile_options(player_unit PRIVATE /W4 /permissive- /EHsc /utf-8)
    add_test(NAME player_unit COMMAND player_unit)
endif()

install(TARGETS WasioPlayer RUNTIME DESTINATION bin)
