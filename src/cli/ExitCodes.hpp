#pragma once

namespace spacelens::cli {

enum class ExitCode : int {
    Success = 0,
    InternalError = 1,
    UsageError = 2,
    InaccessibleRoot = 3,
    ScanFailed = 4,
    Cancelled = 5,
    IndexNotFound = 6,
};

inline int toInt(ExitCode code) noexcept
{
    return static_cast<int>(code);
}

}  // namespace spacelens::cli
