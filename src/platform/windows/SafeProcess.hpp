#pragma once

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

/// Allowlisted, non-shell child process. Never uses cmd.exe or PowerShell.
/// lpApplicationName is the resolved PE path; arguments are passed as argv.
struct SafeProcessRequest {
    std::wstring executableName;  // e.g. L"dotnet.exe"
    std::vector<std::wstring> arguments;
    std::uint32_t timeoutMs = 5000;
    std::size_t maxStdoutBytes = 64 * 1024;
    std::size_t maxStderrBytes = 16 * 1024;
};

enum class SafeProcessStatus {
    Completed,
    TimedOut,
    Cancelled,
    NotFound,
    Failed
};

struct SafeProcessResult {
    SafeProcessStatus status = SafeProcessStatus::Failed;
    std::uint32_t exitCode = 0;
    std::string stdoutUtf8;
    std::string stderrUtf8;
    std::string detail;
};

/// Resolve an allowlisted executable via SearchPathW and run it with
/// CreateProcessW. Paths never enter a shell command string.
[[nodiscard]] SafeProcessResult runAllowlistedProcess(
    const SafeProcessRequest& request, std::stop_token stop = {});

[[nodiscard]] bool isAllowlistedExecutable(std::wstring_view name) noexcept;

}  // namespace spacelens
