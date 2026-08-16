#pragma once

#include <fstream>
#include <string>

namespace wasio {

// Paths are UTF-8 everywhere inside this project (TrackInfo::path, M3U8 files,
// PlayerController). MSVC's narrow std::ifstream/std::ofstream path overloads
// go through the process ANSI code page, so a UTF-8 path holding anything
// outside ASCII opens the wrong file or fails outright. These helpers widen to
// UTF-16 first and use the wide overload, which is the only encoding-safe way
// in on Windows.
std::wstring widen_utf8(const std::string& utf8);
std::string narrow_to_utf8(const std::wstring& wide);

std::ifstream open_utf8_ifstream(const std::string& path, std::ios::openmode mode);
std::ofstream open_utf8_ofstream(const std::string& path, std::ios::openmode mode);

} // namespace wasio
