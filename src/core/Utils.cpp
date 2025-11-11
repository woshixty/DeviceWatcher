#include "core/Utils.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace Utils {

std::string currentTimestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto tt = system_clock::to_time_t(now);

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

bool isWindows() {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

bool isLinux() {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

bool isMac() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

std::string osName() {
    if (isWindows()) return "Windows";
    if (isMac()) return "macOS";
    if (isLinux()) return "Linux";
    return "Unknown";
}

std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::now();
}

std::string formatTimeHHMMSS(const std::chrono::system_clock::time_point& tp) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

} // namespace Utils
