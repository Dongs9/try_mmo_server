#pragma once

#include <iostream>
#include <string_view>

namespace trymmo
{
inline void logInfo(std::string_view message)
{
    std::cout << "[INFO] " << message << std::endl;
}

inline void logWarn(std::string_view message)
{
    std::cerr << "[WARN] " << message << '\n';
}
}
