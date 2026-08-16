#include "asio/driver_registry.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wasio {
namespace {

std::string read_string_value(HKEY key, const char* value_name)
{
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExA(key, value_name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) {
        return {};
    }

    std::string value(bytes, '\0');
    if (RegQueryValueExA(key, value_name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(value.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    value.resize(bytes > 0 ? bytes - 1 : 0);

    if (type == REG_EXPAND_SZ) {
        char expanded[32768] = {};
        const DWORD expanded_size = ExpandEnvironmentStringsA(value.c_str(), expanded,
                                                               static_cast<DWORD>(sizeof(expanded)));
        if (expanded_size > 0 && expanded_size < sizeof(expanded)) {
            return std::string(expanded, expanded_size - 1);
        }
    }
    return value;
}

std::string architecture_for_file(const std::string& path, bool* exists, bool* is_x64)
{
    *exists = false;
    *is_x64 = false;
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return "missing";
    }
    *exists = true;

    DWORD read = 0;
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS64 nt = {};
    LARGE_INTEGER offset = {};
    if (!ReadFile(file, &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        CloseHandle(file);
        return "unknown";
    }

    offset.QuadPart = dos.e_lfanew;
    if (SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) == FALSE ||
        !ReadFile(file, &nt, sizeof(nt), &read, nullptr) ||
        read < sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)) {
        CloseHandle(file);
        return "unknown";
    }
    CloseHandle(file);

    if (nt.Signature != IMAGE_NT_SIGNATURE) {
        return "unknown";
    }

    switch (nt.FileHeader.Machine) {
    case IMAGE_FILE_MACHINE_AMD64:
        *is_x64 = true;
        return "x64";
    case IMAGE_FILE_MACHINE_I386:
        return "x86";
    case IMAGE_FILE_MACHINE_ARM64:
        return "ARM64";
    default:
        return "unknown";
    }
}

} // namespace

std::vector<DriverRecord> enumerate_registered_drivers()
{
    std::vector<DriverRecord> result;
    HKEY asio_key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0,
                      KEY_READ | KEY_WOW64_64KEY, &asio_key) != ERROR_SUCCESS) {
        return result;
    }

    for (DWORD index = 0;; ++index) {
        char key_name[256] = {};
        DWORD key_name_size = static_cast<DWORD>(sizeof(key_name));
        const LONG enum_result = RegEnumKeyExA(asio_key, index, key_name, &key_name_size,
                                                nullptr, nullptr, nullptr, nullptr);
        if (enum_result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (enum_result != ERROR_SUCCESS) {
            continue;
        }

        HKEY driver_key = nullptr;
        if (RegOpenKeyExA(asio_key, key_name, 0, KEY_READ, &driver_key) != ERROR_SUCCESS) {
            continue;
        }

        DriverRecord record;
        record.registry_name = key_name;
        record.description = read_string_value(driver_key, "description");
        record.clsid = read_string_value(driver_key, "clsid");
        RegCloseKey(driver_key);

        if (!record.clsid.empty()) {
            const std::string clsid_path = "SOFTWARE\\Classes\\CLSID\\" + record.clsid +
                                           "\\InprocServer32";
            HKEY inproc_key = nullptr;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, clsid_path.c_str(), 0,
                              KEY_READ | KEY_WOW64_64KEY, &inproc_key) == ERROR_SUCCESS) {
                record.dll_path = read_string_value(inproc_key, "");
                RegCloseKey(inproc_key);
            }
        }

        record.architecture = architecture_for_file(record.dll_path, &record.dll_exists,
                                                      &record.is_x64);
        result.push_back(std::move(record));
    }

    RegCloseKey(asio_key);
    return result;
}

} // namespace wasio
