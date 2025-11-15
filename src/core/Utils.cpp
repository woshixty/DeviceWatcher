#include "core/Utils.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cmath>

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

std::string formatTimeISO8601(const std::chrono::system_clock::time_point& tp) {
    using namespace std::chrono;
    std::time_t tt = system_clock::to_time_t(tp);

    std::tm local{};
    std::tm utc{};
#if defined(_WIN32)
    localtime_s(&local, &tt);
    gmtime_s(&utc, &tt);
#else
    localtime_r(&tt, &local);
    gmtime_r(&tt, &utc);
#endif

    std::time_t local_sec = std::mktime(&local);

#if defined(_WIN32)
    std::time_t utc_sec = _mkgmtime(&utc);
#else
    std::time_t utc_sec = timegm(&utc);
#endif

    long offset_sec = static_cast<long>(std::difftime(local_sec, utc_sec));
    char sign = '+';
    if (offset_sec < 0) {
        sign = '-';
        offset_sec = -offset_sec;
    }
    int offset_h = static_cast<int>(offset_sec / 3600);
    int offset_m = static_cast<int>((offset_sec % 3600) / 60);

    std::ostringstream oss;
    oss << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    oss << sign
        << std::setw(2) << std::setfill('0') << offset_h
        << ':'
        << std::setw(2) << std::setfill('0') << offset_m;
    return oss.str();
}

} // namespace Utils
