#pragma once

#include <string>
#include <chrono>

namespace Utils {

// Return local timestamp formatted as YYYY-MM-DD HH:MM:SS
std::string currentTimestamp();

// OS detection helpers
bool isWindows();
bool isLinux();
bool isMac();
std::string osName();

// Monotonic clock now (for debouncing, timeouts)
std::chrono::steady_clock::time_point now();

// Format a system_clock time_point to HH:MM:SS
std::string formatTimeHHMMSS(const std::chrono::system_clock::time_point& tp);

// Format a system_clock time_point to ISO8601 local time, e.g. 2025-11-09T22:07:02+08:00
std::string formatTimeISO8601(const std::chrono::system_clock::time_point& tp);

} // namespace Utils
