#include "formats/utf8_file.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace wasio {

std::wstring widen_utf8(const std::string& utf8)
{
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(),
                        needed);
    return result;
}

std::string narrow_to_utf8(const std::wstring& wide)
{
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                           static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), result.data(),
                        needed, nullptr, nullptr);
    return result;
}

std::ifstream open_utf8_ifstream(const std::string& path, std::ios::openmode mode)
{
    return std::ifstream(widen_utf8(path).c_str(), mode);
}

std::ofstream open_utf8_ofstream(const std::string& path, std::ios::openmode mode)
{
    return std::ofstream(widen_utf8(path).c_str(), mode);
}

} // namespace wasio
