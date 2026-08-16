#pragma once

#include <string>
#include <vector>

namespace wasio {

struct DriverRecord {
    std::string registry_name;
    std::string description;
    std::string clsid;
    std::string dll_path;
    std::string architecture;
    bool dll_exists = false;
    bool is_x64 = false;
};

std::vector<DriverRecord> enumerate_registered_drivers();

} // namespace wasio
