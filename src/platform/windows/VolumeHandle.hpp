#pragma once

#include <cstdint>
#include <string>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace spacelens {

/// Move-only RAII for a Win32 volume/device HANDLE (CloseHandle on destroy).
class VolumeHandle {
public:
    VolumeHandle() = default;
    explicit VolumeHandle(HANDLE handle) noexcept
        : m_handle(handle)
    {
    }

    ~VolumeHandle() { reset(); }

    VolumeHandle(const VolumeHandle&) = delete;
    VolumeHandle& operator=(const VolumeHandle&) = delete;

    VolumeHandle(VolumeHandle&& other) noexcept
        : m_handle(other.m_handle)
    {
        other.m_handle = INVALID_HANDLE_VALUE;
    }

    VolumeHandle& operator=(VolumeHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (valid()) {
            ::CloseHandle(m_handle);
        }
        m_handle = handle;
    }

    [[nodiscard]] HANDLE release() noexcept
    {
        HANDLE h = m_handle;
        m_handle = INVALID_HANDLE_VALUE;
        return h;
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

struct VolumeIdentity {
    std::wstring devicePath;    // e.g. "\\.\C:"
    std::wstring rootPath;      // e.g. "C:\"
    std::uint32_t serialNumber = 0;
    std::wstring fileSystem;    // NTFS, ReFS, ...
    bool isNtfs = false;
};

/// Resolve volume identity for any path (file or directory). Does not open the journal.
[[nodiscard]] bool resolveVolumeIdentity(const std::wstring& anyPath,
                                         VolumeIdentity& out);

/// Open a volume for read-only DeviceIoControl (USN query/read).
/// Uses GENERIC_READ only — never write access to the volume.
[[nodiscard]] VolumeHandle openVolumeReadOnly(const VolumeIdentity& volume);

}  // namespace spacelens
