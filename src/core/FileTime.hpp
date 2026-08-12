#pragma once

#include <cstdint>

namespace spacelens {

/// Windows FILETIME-compatible 100ns ticks since 1601-01-01 UTC.
using FileTimeTicks = std::uint64_t;

inline constexpr FileTimeTicks kFileTimeTicksPerSecond = 10'000'000ULL;
inline constexpr FileTimeTicks kFileTimeTicksPerDay =
    kFileTimeTicksPerSecond * 60ULL * 60ULL * 24ULL;

/// Convert whole days to FILETIME tick delta.
[[nodiscard]] inline FileTimeTicks daysToTicks(std::uint64_t days) noexcept
{
    return days * kFileTimeTicksPerDay;
}

/// Age in whole days of `then` relative to `now`. Returns 0 if unknown or future.
[[nodiscard]] inline std::uint64_t ageDays(FileTimeTicks then,
                                           FileTimeTicks now) noexcept
{
    if (then == 0 || now == 0 || then > now) {
        return 0;
    }
    return (now - then) / kFileTimeTicksPerDay;
}

/// True when `then` is strictly older than `days` relative to `now`.
[[nodiscard]] inline bool isOlderThanDays(FileTimeTicks then,
                                          FileTimeTicks now,
                                          std::uint64_t days) noexcept
{
    if (then == 0 || now == 0) {
        return false;
    }
    return ageDays(then, now) >= days;
}

}  // namespace spacelens
