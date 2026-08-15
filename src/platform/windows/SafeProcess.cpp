#include "platform/windows/SafeProcess.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <vector>

namespace spacelens {
namespace {

constexpr std::wstring_view kAllowlisted[] = {
    L"dotnet.exe",
};

bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) {
            return false;
        }
    }
    return true;
}

std::wstring quoteArg(const std::wstring& arg)
{
    // CreateProcessW requires a mutable command line. Quote arguments that
    // contain whitespace. Paths come only from SearchPath / our argv, never
    // from a user-constructed shell string.
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out;
    out.push_back(L'"');
    for (wchar_t ch : arg) {
        if (ch == L'"') {
            out.append(L"\\\"");
        } else {
            out.push_back(ch);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring resolveAllowlisted(const std::wstring& name)
{
    wchar_t buffer[MAX_PATH]{};
    const DWORD n = ::SearchPathW(nullptr, name.c_str(), nullptr, MAX_PATH,
                                  buffer, nullptr);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    return std::wstring(buffer, n);
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept
        : m_handle(h)
    {
    }
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept
        : m_handle(other.m_handle)
    {
        other.m_handle = nullptr;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] HANDLE* write() noexcept { return &m_handle; }
    void reset() noexcept
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_handle);
        }
        m_handle = nullptr;
    }

private:
    HANDLE m_handle = nullptr;
};

std::string readPipeLimited(HANDLE pipe, std::size_t maxBytes)
{
    std::string out;
    out.reserve(std::min<std::size_t>(maxBytes, 4096));
    char buf[1024];
    DWORD got = 0;
    while (out.size() < maxBytes) {
        const DWORD want = static_cast<DWORD>(
            std::min<std::size_t>(sizeof(buf), maxBytes - out.size()));
        if (!::ReadFile(pipe, buf, want, &got, nullptr) || got == 0) {
            break;
        }
        out.append(buf, buf + got);
    }
    return out;
}

}  // namespace

bool isAllowlistedExecutable(std::wstring_view name) noexcept
{
    const auto slash = name.find_last_of(L"\\/");
    const std::wstring_view leaf =
        slash == std::wstring_view::npos ? name : name.substr(slash + 1);
    for (const auto allowed : kAllowlisted) {
        if (equalsIgnoreCase(leaf, allowed)) {
            return true;
        }
    }
    return false;
}

SafeProcessResult runAllowlistedProcess(const SafeProcessRequest& request,
                                        std::stop_token stop)
{
    SafeProcessResult result;
    if (!isAllowlistedExecutable(request.executableName)) {
        result.status = SafeProcessStatus::Failed;
        result.detail = "executable_not_allowlisted";
        return result;
    }
    if (stop.stop_requested()) {
        result.status = SafeProcessStatus::Cancelled;
        result.detail = "cancelled";
        return result;
    }

    const std::wstring resolved = resolveAllowlisted(request.executableName);
    if (resolved.empty()) {
        result.status = SafeProcessStatus::NotFound;
        result.detail = "executable_not_found";
        return result;
    }
    if (!isAllowlistedExecutable(resolved)) {
        result.status = SafeProcessStatus::Failed;
        result.detail = "resolved_path_not_allowlisted";
        return result;
    }

    std::wstring commandLine = quoteArg(resolved);
    for (const auto& arg : request.arguments) {
        commandLine.push_back(L' ');
        commandLine.append(quoteArg(arg));
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rawOutRead = nullptr;
    HANDLE rawOutWrite = nullptr;
    HANDLE rawErrRead = nullptr;
    HANDLE rawErrWrite = nullptr;
    if (!::CreatePipe(&rawOutRead, &rawOutWrite, &sa, 0) ||
        !::CreatePipe(&rawErrRead, &rawErrWrite, &sa, 0)) {
        if (rawOutRead) {
            ::CloseHandle(rawOutRead);
        }
        if (rawOutWrite) {
            ::CloseHandle(rawOutWrite);
        }
        if (rawErrRead) {
            ::CloseHandle(rawErrRead);
        }
        if (rawErrWrite) {
            ::CloseHandle(rawErrWrite);
        }
        result.status = SafeProcessStatus::Failed;
        result.detail = "create_pipe_failed";
        return result;
    }
    UniqueHandle outRead(rawOutRead);
    UniqueHandle outWrite(rawOutWrite);
    UniqueHandle errRead(rawErrRead);
    UniqueHandle errWrite(rawErrWrite);
    ::SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = outWrite.get();
    si.hStdError = errWrite.get();
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back(L'\0');

    const BOOL created = ::CreateProcessW(
        resolved.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si,
        &pi);
    outWrite.reset();
    errWrite.reset();
    if (!created) {
        result.status = SafeProcessStatus::Failed;
        result.detail = "create_process_failed";
        return result;
    }
    UniqueHandle process(pi.hProcess);
    UniqueHandle thread(pi.hThread);

    const DWORD timeout = request.timeoutMs == 0 ? 5000 : request.timeoutMs;
    DWORD wait = WAIT_TIMEOUT;
    const DWORD slice = 50;
    DWORD waited = 0;
    while (waited < timeout) {
        if (stop.stop_requested()) {
            ::TerminateProcess(process.get(), 1);
            result.status = SafeProcessStatus::Cancelled;
            result.detail = "cancelled";
            result.stdoutUtf8 =
                readPipeLimited(outRead.get(), request.maxStdoutBytes);
            result.stderrUtf8 =
                readPipeLimited(errRead.get(), request.maxStderrBytes);
            return result;
        }
        wait = ::WaitForSingleObject(process.get(), slice);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        waited += slice;
    }
    if (wait != WAIT_OBJECT_0) {
        ::TerminateProcess(process.get(), 1);
        result.status = SafeProcessStatus::TimedOut;
        result.detail = "timeout";
        result.stdoutUtf8 =
            readPipeLimited(outRead.get(), request.maxStdoutBytes);
        result.stderrUtf8 =
            readPipeLimited(errRead.get(), request.maxStderrBytes);
        return result;
    }

    DWORD exitCode = 0;
    ::GetExitCodeProcess(process.get(), &exitCode);
    result.exitCode = exitCode;
    result.stdoutUtf8 = readPipeLimited(outRead.get(), request.maxStdoutBytes);
    result.stderrUtf8 = readPipeLimited(errRead.get(), request.maxStderrBytes);
    result.status = SafeProcessStatus::Completed;
    result.detail = "ok";
    return result;
}

}  // namespace spacelens
