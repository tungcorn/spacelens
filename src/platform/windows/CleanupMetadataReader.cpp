#include "platform/windows/CleanupMetadataReader.hpp"

#include "core/CleanupReview.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <cstring>
#include <utility>

namespace spacelens {
namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept
        : m_handle(handle)
    {
    }

    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : m_handle(other.m_handle)
    {
        other.m_handle = INVALID_HANDLE_VALUE;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
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

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

std::uint64_t fileTimeToU64(const FILETIME& ft)
{
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

std::array<std::uint8_t, 16> copyFileId128(const FILE_ID_128& id)
{
    std::array<std::uint8_t, 16> out{};
    std::memcpy(out.data(), id.Identifier, out.size());
    return out;
}

ItemKind kindFrom(bool isDirectory, bool isReparse) noexcept
{
    if (isDirectory && isReparse) {
        return ItemKind::ReparseDirectory;
    }
    if (isDirectory) {
        return ItemKind::Directory;
    }
    return ItemKind::File;
}

CleanupMetadataProbeOutcome mapOpenError(DWORD error) noexcept
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return CleanupMetadataProbeOutcome::Missing;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return CleanupMetadataProbeOutcome::AccessDenied;
    default:
        return CleanupMetadataProbeOutcome::ProbeError;
    }
}

}  // namespace

CleanupMetadataProbe WindowsCleanupMetadataReader::read(const std::wstring& path)
{
    CleanupMetadataProbe probe;
    if (path.empty()) {
        probe.outcome = CleanupMetadataProbeOutcome::ProbeError;
        probe.detail = "Empty path";
        return probe;
    }

    // Same no-follow open as queryFileIdentity(): attributes only, share delete,
    // backup semantics for directories, and OPEN_REPARSE_POINT so the final
    // component is not followed.
    UniqueHandle handle(::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) {
        const DWORD err = ::GetLastError();
        probe.nativeError = static_cast<std::uint32_t>(err);
        probe.outcome = mapOpenError(err);
        probe.detail = "CreateFileW failed";
        return probe;
    }

    probe.outcome = CleanupMetadataProbeOutcome::Present;

    FILE_ID_INFO idInfo{};
    const bool haveFileId =
        ::GetFileInformationByHandleEx(handle.get(), FileIdInfo, &idInfo,
                                       sizeof(idInfo)) != 0;

    BY_HANDLE_FILE_INFORMATION byHandle{};
    const bool haveByHandle =
        ::GetFileInformationByHandle(handle.get(), &byHandle) != 0;

    FILE_BASIC_INFO basic{};
    const bool haveBasic =
        ::GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic,
                                       sizeof(basic)) != 0;

    FILE_STANDARD_INFO standard{};
    const bool haveStandard =
        ::GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard,
                                       sizeof(standard)) != 0;

    std::uint32_t attrs = 0;
    FileTimeTicks writeTime = 0;
    FileTimeTicks accessTime = 0;
    ByteSize size = 0;
    bool isDirectory = false;
    bool haveMeta = false;

    if (haveByHandle) {
        attrs = byHandle.dwFileAttributes;
        writeTime = fileTimeToU64(byHandle.ftLastWriteTime);
        accessTime = fileTimeToU64(byHandle.ftLastAccessTime);
        ULARGE_INTEGER sz;
        sz.HighPart = byHandle.nFileSizeHigh;
        sz.LowPart = byHandle.nFileSizeLow;
        size = sz.QuadPart;
        isDirectory = (byHandle.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        haveMeta = true;
    }
    if (haveBasic) {
        attrs = static_cast<std::uint32_t>(basic.FileAttributes);
        writeTime = static_cast<FileTimeTicks>(basic.LastWriteTime.QuadPart);
        accessTime = static_cast<FileTimeTicks>(basic.LastAccessTime.QuadPart);
        isDirectory = (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        haveMeta = true;
    }
    if (haveStandard) {
        // Direct object size only. For directories this is handle metadata,
        // never a recursive descendant aggregate.
        size = static_cast<ByteSize>(standard.EndOfFile.QuadPart);
        isDirectory = standard.Directory != FALSE;
        haveMeta = true;
    }

    auto& evidence = probe.objectEvidence;
    evidence.sizeScope = CleanupEvidenceScope::Direct;

    if (haveMeta) {
        probe.isReparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        evidence.available = true;
        evidence.kind = kindFrom(isDirectory, probe.isReparse);
        evidence.logicalSize = size;
        evidence.lastWriteTime = writeTime;
        evidence.lastAccessTime = accessTime;
        evidence.attributes = attrs;
    }

    if (haveFileId && evidence.available) {
        evidence.identity =
            makeFileId128Identity(idInfo.VolumeSerialNumber,
                                  copyFileId128(idInfo.FileId));
    } else if (haveByHandle && evidence.available) {
        ULARGE_INTEGER idx;
        idx.HighPart = byHandle.nFileIndexHigh;
        idx.LowPart = byHandle.nFileIndexLow;
        evidence.identity = makeFileIndex64FallbackIdentity(
            byHandle.dwVolumeSerialNumber, idx.QuadPart);
    }

    if (!evidence.available) {
        // The path opened, so this is not Missing. Incomplete metadata is an
        // access/query failure, never a type/content change.
        const DWORD err = ::GetLastError();
        probe.nativeError = static_cast<std::uint32_t>(err);
        probe.detail = "File information query failed";
        probe.outcome =
            (err == ERROR_ACCESS_DENIED || err == ERROR_PRIVILEGE_NOT_HELD)
                ? CleanupMetadataProbeOutcome::AccessDenied
                : CleanupMetadataProbeOutcome::ProbeError;
    }

    return probe;
}

}  // namespace spacelens
