#include "platform/windows/VolumeHandle.hpp"

#include <cctype>
#include <cwctype>

namespace spacelens {
namespace {

std::wstring toUpperDrive(wchar_t letter)
{
    std::wstring s;
    s.push_back(static_cast<wchar_t>(std::towupper(letter)));
    s.push_back(L':');
    return s;
}

/// Enable a privilege already present on the process token (best-effort).
/// Does not elevate; only flips SE_PRIVILEGE_ENABLED when the account has the
/// privilege assigned (Administrators / Backup Operators).
bool tryEnablePrivilege(const wchar_t* privilegeName)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!::LookupPrivilegeValueW(nullptr, privilegeName, &tp.Privileges[0].Luid)) {
        ::CloseHandle(token);
        return false;
    }

    ::AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = ::GetLastError();
    ::CloseHandle(token);
    // AdjustTokenPrivileges can return TRUE even when not all privileges were
    // assigned — ERROR_NOT_ALL_ASSIGNED means the privilege is absent.
    return err != ERROR_NOT_ALL_ASSIGNED && err != ERROR_PRIVILEGE_NOT_HELD;
}

HANDLE tryCreateVolumeHandle(const std::wstring& devicePath, DWORD access,
                             DWORD flags)
{
    return ::CreateFileW(devicePath.c_str(), access,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, flags, nullptr);
}

}  // namespace

bool resolveVolumeIdentity(const std::wstring& anyPath, VolumeIdentity& out)
{
    out = {};
    if (anyPath.empty()) {
        return false;
    }

    wchar_t volumePath[MAX_PATH]{};
    if (!::GetVolumePathNameW(anyPath.c_str(), volumePath, MAX_PATH)) {
        // Drive-letter fallback: "D:\foo" or "D:"
        if (anyPath.size() >= 2 && anyPath[1] == L':') {
            const std::wstring root = toUpperDrive(anyPath[0]) + L"\\";
            out.rootPath = root;
            out.devicePath = L"\\\\.\\" + toUpperDrive(anyPath[0]);
        } else {
            return false;
        }
    } else {
        out.rootPath = volumePath;
        // Build device path from drive-letter volume root.
        if (out.rootPath.size() >= 2 && out.rootPath[1] == L':') {
            out.devicePath = L"\\\\.\\" + toUpperDrive(out.rootPath[0]);
        } else {
            // Mount-point / volume GUID paths: use GetVolumeNameForVolumeMountPoint
            wchar_t name[MAX_PATH]{};
            if (!::GetVolumeNameForVolumeMountPointW(out.rootPath.c_str(), name,
                                                    MAX_PATH)) {
                return false;
            }
            // Convert \\?\Volume{guid}\ form to \\.\Volume{guid}
            std::wstring n = name;
            if (n.rfind(L"\\\\?\\", 0) == 0) {
                n.replace(0, 4, L"\\\\.\\");
            }
            if (!n.empty() && n.back() == L'\\') {
                n.pop_back();
            }
            out.devicePath = std::move(n);
        }
    }

    wchar_t fsName[MAX_PATH]{};
    DWORD serial = 0;
    DWORD maxComp = 0;
    DWORD fsFlags = 0;
    if (!::GetVolumeInformationW(out.rootPath.c_str(), nullptr, 0, &serial,
                                 &maxComp, &fsFlags, fsName, MAX_PATH)) {
        // Still return device path; mark non-NTFS if unknown.
        out.serialNumber = 0;
        out.fileSystem.clear();
        out.isNtfs = false;
        return !out.devicePath.empty();
    }

    out.serialNumber = serial;
    out.fileSystem = fsName;
    for (auto& ch : out.fileSystem) {
        ch = static_cast<wchar_t>(std::towupper(ch));
    }
    out.isNtfs = (out.fileSystem == L"NTFS");
    return true;
}

VolumeHandle openVolumeReadOnly(const VolumeIdentity& volume)
{
    if (volume.devicePath.empty()) {
        return VolumeHandle{};
    }

    // Volume handles typically require SeBackupPrivilege or elevation.
    // Enable if present; never request write access to the volume.
    tryEnablePrivilege(SE_BACKUP_NAME);
    tryEnablePrivilege(SE_RESTORE_NAME);

    // Prefer real read access. A zero-access CreateFile can succeed on some
    // Windows builds while DeviceIoControl(USN) still fails — callers must
    // still treat query failures as access denied, not "non-NTFS".
    static constexpr DWORD kAccess[] = {
        GENERIC_READ,
        FILE_READ_DATA | FILE_READ_ATTRIBUTES,
        FILE_READ_ATTRIBUTES,
    };
    static constexpr DWORD kFlags[] = {
        FILE_FLAG_BACKUP_SEMANTICS,
        FILE_ATTRIBUTE_NORMAL,
    };

    for (const DWORD access : kAccess) {
        for (const DWORD flags : kFlags) {
            HANDLE h = tryCreateVolumeHandle(volume.devicePath, access, flags);
            if (h != INVALID_HANDLE_VALUE && h != nullptr) {
                return VolumeHandle{h};
            }
        }
    }
    // Preserve last CreateFile failure in GetLastError for callers.
    return VolumeHandle{};
}

}  // namespace spacelens
