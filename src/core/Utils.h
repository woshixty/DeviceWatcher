#pragma once

#include <string>

namespace Utils {

// Return local timestamp formatted as YYYY-MM-DD HH:MM:SS
std::string currentTimestamp();

// OS detection helpers
bool isWindows();
bool isLinux();
bool isMac();
std::string osName();

} // namespace Utils

