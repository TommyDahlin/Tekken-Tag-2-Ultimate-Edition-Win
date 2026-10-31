#pragma once
#include <cstdint>
#include <ctime>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace cafe::host {
// Offset at the supplied instant, including daylight saving time.
#ifdef _WIN32
inline int64_t utc_offset_seconds(std::time_t instant, const TIME_ZONE_INFORMATION* zone = nullptr) {
    constexpr int64_t epoch = 11644473600LL;
    if (instant < -epoch || instant > 910670515199LL)
        throw std::runtime_error("local time is outside Windows' supported range");
    ULARGE_INTEGER ticks{};
    ticks.QuadPart = static_cast<uint64_t>(instant + epoch) * 10000000ULL;
    FILETIME utc_file{ticks.LowPart, ticks.HighPart}, local_file{};
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&utc_file, &utc) ||
        !SystemTimeToTzSpecificLocalTime(zone, &utc, &local) ||
        !SystemTimeToFileTime(&local, &local_file))
        throw std::runtime_error("cannot convert Windows local time");
    ULARGE_INTEGER local_ticks{};
    local_ticks.LowPart = local_file.dwLowDateTime;
    local_ticks.HighPart = local_file.dwHighDateTime;
    return (static_cast<int64_t>(local_ticks.QuadPart) - static_cast<int64_t>(ticks.QuadPart)) / 10000000LL;
}
#else
inline int64_t utc_offset_seconds(std::time_t instant) {
    std::tm local{};
    if (!localtime_r(&instant, &local))
        throw std::runtime_error("cannot convert local time");
    return local.tm_gmtoff;
}
#endif
} // namespace cafe::host
