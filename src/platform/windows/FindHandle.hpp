#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <utility>

namespace spacelens::win {

/// RAII owner for a FindFirstFileExW search handle.
/// Closes with FindClose (not CloseHandle).
class FindHandle {
public:
    FindHandle() = default;
    explicit FindHandle(HANDLE handle) noexcept
        : m_handle(handle)
    {
    }

    ~FindHandle() { reset(); }

    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;

    FindHandle(FindHandle&& other) noexcept
        : m_handle(other.m_handle)
    {
        other.m_handle = INVALID_HANDLE_VALUE;
    }

    FindHandle& operator=(FindHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr;
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr) {
            ::FindClose(m_handle);
        }
        m_handle = handle;
    }

    HANDLE release() noexcept
    {
        return std::exchange(m_handle, INVALID_HANDLE_VALUE);
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

}  // namespace spacelens::win
