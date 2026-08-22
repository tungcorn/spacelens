#include "platform/windows/FileIdentity.hpp"

#include "core/SafetyPolicy.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstddef>
#include <cwctype>
#include <cstring>
#include <limits>
#include <vector>

namespace spacelens {
namespace {

std::uint64_t fileTimeToU64(const FILETIME& ft)
{
    ULARGE_INTEGER v;
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return v.QuadPart;
}

std::wstring toLowerCopy(std::wstring s)
{
    for (wchar_t& ch : s) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return s;
}

std::wstring stripExtendedPrefix(std::wstring p)
{
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        p.replace(0, 8, L"\\\\");
    } else if (p.rfind(L"\\\\?\\", 0) == 0) {
        p.erase(0, 4);
    }
    return p;
}

std::wstring expandFullPath(std::wstring_view input)
{
    if (input.empty()) {
        return {};
    }
    std::wstring in(input);
    const DWORD need = ::GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
    if (need == 0) {
        return {};
    }
    std::wstring buf(need, L'\0');
    const DWORD got = ::GetFullPathNameW(in.c_str(), need, buf.data(), nullptr);
    if (got == 0 || got >= need) {
        return {};
    }
    buf.resize(got);
    return buf;
}

std::wstring expandLongPath(std::wstring_view input)
{
    if (input.empty()) {
        return {};
    }
    std::wstring in(input);
    const DWORD need = ::GetLongPathNameW(in.c_str(), nullptr, 0);
    if (need == 0) {
        return {};
    }
    std::wstring buf(need, L'\0');
    const DWORD got = ::GetLongPathNameW(in.c_str(), buf.data(), need);
    if (got == 0 || got >= need) {
        return {};
    }
    buf.resize(got);
    return buf;
}

}  // namespace

std::wstring canonicalWin32Path(std::wstring_view path)
{
    if (path.empty()) {
        return {};
    }

    std::wstring expanded = expandFullPath(path);
    if (expanded.empty()) {
        expanded.assign(path);
    }

    if (const std::wstring lng = expandLongPath(expanded); !lng.empty()) {
        expanded = lng;
    }

    return normalizePathForPolicy(stripExtendedPrefix(std::move(expanded)));
}

bool win32PathsEqual(std::wstring_view a, std::wstring_view b)
{
    if (a.empty() || b.empty()) {
        return a.empty() && b.empty();
    }
    return toLowerCopy(canonicalWin32Path(a)) == toLowerCopy(canonicalWin32Path(b));
}

std::wstring rebasePathOntoRoot(const std::wstring& path, const std::wstring& root)
{
    if (path.empty()) {
        return {};
    }
    if (root.empty()) {
        return normalizePathForPolicy(path);
    }

    const std::wstring canonPath = canonicalWin32Path(path);
    const std::wstring canonRoot = canonicalWin32Path(root);
    if (canonPath.empty() || canonRoot.empty()) {
        return normalizePathForPolicy(path);
    }

    const std::wstring p = toLowerCopy(canonPath);
    const std::wstring r = toLowerCopy(canonRoot);
    if (p == r) {
        return normalizePathForPolicy(root);
    }
    if (p.size() > r.size() && p.compare(0, r.size(), r) == 0) {
        const bool rootHasSep = (r.back() == L'\\' || r.back() == L'/');
        const bool atSep = (p[r.size()] == L'\\' || p[r.size()] == L'/');
        if (rootHasSep || atSep) {
            return normalizePathForPolicy(root) + canonPath.substr(canonRoot.size());
        }
    }
    return normalizePathForPolicy(path);
}

namespace {

bool isZeroFileId(const FILE_ID_128& id) noexcept
{
    for (const unsigned char byte : id.Identifier) {
        if (byte != 0U) {
            return false;
        }
    }
    return true;
}

bool fileTimesEqual(const FILETIME& left, const FILETIME& right) noexcept
{
    return left.dwLowDateTime == right.dwLowDateTime &&
           left.dwHighDateTime == right.dwHighDateTime;
}

struct FileStreamAllocation {
    ByteSize total = 0;
    ByteSize unnamed = 0;
    bool unnamedKnown = false;
};

constexpr std::size_t kInitialFileStreamInfoBufferBytes = 4U * 1024U;
constexpr std::size_t kMaximumFileStreamInfoBufferBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumFileStreamInfoRecords = 131072U;

std::optional<FileStreamAllocation> enumerateFileStreamAllocation(HANDLE handle)
{
    constexpr std::size_t headerBytes = offsetof(FILE_STREAM_INFO, StreamName);
    std::array<std::uint8_t, kInitialFileStreamInfoBufferBytes> stackBuffer;
    std::vector<std::uint8_t> dynamicBuffer;
    std::uint8_t* buffer = stackBuffer.data();
    std::size_t bufferSize = stackBuffer.size();

    for (;;) {
        if (::GetFileInformationByHandleEx(
                handle, FileStreamInfo, buffer,
                static_cast<DWORD>(bufferSize)) != FALSE) {
            break;
        }

        const DWORD error = ::GetLastError();
        if (error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER &&
            error != ERROR_BUFFER_OVERFLOW) {
            return std::nullopt;
        }
        if (bufferSize >= kMaximumFileStreamInfoBufferBytes) {
            return std::nullopt;
        }
        const std::size_t nextSize =
            bufferSize > kMaximumFileStreamInfoBufferBytes / 2U
                ? kMaximumFileStreamInfoBufferBytes
                : bufferSize * 2U;
        if (nextSize <= bufferSize ||
            nextSize > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
            return std::nullopt;
        }
        dynamicBuffer.resize(nextSize);
        buffer = dynamicBuffer.data();
        bufferSize = dynamicBuffer.size();
    }

    FileStreamAllocation result;
    std::size_t offset = 0;
    std::size_t recordCount = 0;
    for (;;) {
        if (offset > bufferSize || bufferSize - offset < headerBytes ||
            ++recordCount > kMaximumFileStreamInfoRecords) {
            return std::nullopt;
        }

        FILE_STREAM_INFO info{};
        std::memcpy(&info, buffer + offset, headerBytes);
        const std::size_t remaining = bufferSize - offset;
        const std::size_t nameBytes =
            static_cast<std::size_t>(info.StreamNameLength);
        if (nameBytes == 0 || nameBytes % sizeof(wchar_t) != 0 ||
            nameBytes > remaining - headerBytes) {
            return std::nullopt;
        }

        std::size_t recordBytes = remaining;
        if (info.NextEntryOffset != 0) {
            const std::size_t nextOffset =
                static_cast<std::size_t>(info.NextEntryOffset);
            if (nextOffset < headerBytes || nextOffset > remaining ||
                nextOffset % alignof(FILE_STREAM_INFO) != 0 ||
                nameBytes > nextOffset - headerBytes) {
                return std::nullopt;
            }
            recordBytes = nextOffset;
        }
        if (nameBytes > recordBytes - headerBytes ||
            info.StreamAllocationSize.QuadPart < 0 ||
            info.StreamSize.QuadPart < 0) {
            return std::nullopt;
        }

        std::wstring streamName(nameBytes / sizeof(wchar_t), L'\0');
        std::memcpy(streamName.data(), buffer + offset + headerBytes, nameBytes);
        for (const wchar_t ch : streamName) {
            if (ch == L'\0') {
                return std::nullopt;
            }
        }

        const ByteSize allocation = static_cast<ByteSize>(
            info.StreamAllocationSize.QuadPart);
        if (allocation > std::numeric_limits<ByteSize>::max() - result.total) {
            return std::nullopt;
        }
        result.total += allocation;
        if (streamName == L"::$DATA") {
            if (result.unnamedKnown) {
                return std::nullopt;
            }
            result.unnamed = allocation;
            result.unnamedKnown = true;
        }

        if (info.NextEntryOffset == 0) {
            return result.unnamedKnown ? std::optional<FileStreamAllocation>(result)
                                       : std::nullopt;
        }
        offset += static_cast<std::size_t>(info.NextEntryOffset);
    }
}

}  // namespace

namespace {

std::optional<FileIdentity> queryFileIdentityFromHandleImpl(
    void* rawHandle, bool includeAllocation)
{
    const HANDLE h = static_cast<HANDLE>(rawHandle);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    BY_HANDLE_FILE_INFORMATION byHandle{};
    if (::GetFileInformationByHandle(h, &byHandle) == FALSE) {
        return std::nullopt;
    }

    FILE_STANDARD_INFO standard{};
    const bool haveStandard =
        ::GetFileInformationByHandleEx(h, FileStandardInfo, &standard,
                                       sizeof(standard)) != 0;
    FILE_BASIC_INFO basic{};
    const bool haveBasic =
        ::GetFileInformationByHandleEx(h, FileBasicInfo, &basic,
                                       sizeof(basic)) != 0;
    FILE_ID_INFO idInfo{};
    const bool haveFileIdInfo =
        ::GetFileInformationByHandleEx(h, FileIdInfo, &idInfo,
                                       sizeof(idInfo)) != 0;

    FileIdentity id;
    ULARGE_INTEGER fileIndex;
    fileIndex.HighPart = byHandle.nFileIndexHigh;
    fileIndex.LowPart = byHandle.nFileIndexLow;
    id.fileId = fileIndex.QuadPart;
    id.volumeSerial = byHandle.dwVolumeSerialNumber;
    id.fileIndex64Known = id.fileId != 0 && id.volumeSerial != 0;

    if (haveFileIdInfo && idInfo.VolumeSerialNumber != 0 &&
        !isZeroFileId(idInfo.FileId)) {
        id.volumeSerial = idInfo.VolumeSerialNumber;
        std::memcpy(id.fileId128.data(), idInfo.FileId.Identifier,
                    id.fileId128.size());
        id.fileId128Known = true;
    }
    id.identityKnown = id.fileId128Known || id.fileIndex64Known;

    const bool byHandleDirectory =
        (byHandle.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    id.isDirectory = byHandleDirectory;
    ULARGE_INTEGER byHandleSize;
    byHandleSize.HighPart = byHandle.nFileSizeHigh;
    byHandleSize.LowPart = byHandle.nFileSizeLow;
    id.sizeBytes = byHandleSize.QuadPart;
    id.logicalSizeKnown = true;
    id.numberOfLinks = byHandle.nNumberOfLinks;
    id.linkCountKnown = true;
    id.attributes = byHandle.dwFileAttributes;
    id.sparse = (byHandle.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
    id.compressed =
        (byHandle.dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0;
    id.lastWriteTicks = fileTimeToU64(byHandle.ftLastWriteTime);
    id.lastAccessTicks = fileTimeToU64(byHandle.ftLastAccessTime);

    if (haveStandard) {
        if (standard.EndOfFile.QuadPart >= 0) {
            const ByteSize standardSize =
                static_cast<ByteSize>(standard.EndOfFile.QuadPart);
            if (standardSize != id.sizeBytes) {
                id.observationConsistent = false;
            }
            id.sizeBytes = standardSize;
            id.logicalSizeKnown = true;
        } else {
            id.logicalSizeKnown = false;
        }
        if (standard.NumberOfLinks != id.numberOfLinks) {
            id.observationConsistent = false;
        }
        id.numberOfLinks = standard.NumberOfLinks;
        id.linkCountKnown = true;
        if ((standard.Directory != FALSE) != byHandleDirectory) {
            id.observationConsistent = false;
        }
        id.isDirectory = standard.Directory != FALSE;
    }

    if (includeAllocation) {
        if (id.isDirectory) {
            if (haveStandard && standard.AllocationSize.QuadPart >= 0) {
                id.allocatedBytes =
                    static_cast<ByteSize>(standard.AllocationSize.QuadPart);
                id.allocationKnown = true;
            }
        } else {
            const auto streamAllocation = enumerateFileStreamAllocation(h);
            if (streamAllocation.has_value()) {
                if (haveStandard && standard.AllocationSize.QuadPart >= 0 &&
                    streamAllocation->unnamed !=
                        static_cast<ByteSize>(standard.AllocationSize.QuadPart)) {
                    // FILE_STANDARD_INFO describes the unnamed stream only. A
                    // mismatch means the two metadata snapshots cannot be used as
                    // one deterministic observation.
                    id.observationConsistent = false;
                } else {
                    id.allocatedBytes = streamAllocation->total;
                    id.allocationKnown = true;
                }
            }
        }
    }

    if (haveBasic) {
        const std::uint32_t basicAttributes =
            static_cast<std::uint32_t>(basic.FileAttributes);
        const FILETIME basicLastWriteTime{
            static_cast<DWORD>(basic.LastWriteTime.LowPart),
            static_cast<DWORD>(basic.LastWriteTime.HighPart)};
        const FILETIME basicLastAccessTime{
            static_cast<DWORD>(basic.LastAccessTime.LowPart),
            static_cast<DWORD>(basic.LastAccessTime.HighPart)};
        if (basicAttributes != id.attributes ||
            !fileTimesEqual(basicLastWriteTime, byHandle.ftLastWriteTime)) {
            id.observationConsistent = false;
        }
        // Last-access time is lazy filesystem metadata and may advance merely
        // because this read-only handle was opened. A difference between the
        // two same-handle queries is therefore not evidence that file content,
        // identity, or namespace state changed.
        id.attributes = basicAttributes;
        id.sparse = (basicAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
        id.compressed =
            (basicAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0;
        id.creationTimeTicks =
            static_cast<std::uint64_t>(basic.CreationTime.QuadPart);
        id.changeTimeTicks =
            static_cast<std::uint64_t>(basic.ChangeTime.QuadPart);
        id.lastWriteTicks = static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart);
        id.basicMetadataKnown = true;
        if (fileTimesEqual(basicLastAccessTime, byHandle.ftLastAccessTime)) {
            id.lastAccessTicks =
                static_cast<std::uint64_t>(basic.LastAccessTime.QuadPart);
        }
        id.isDirectory = (basicAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    return id;
}

}  // namespace

std::optional<FileIdentity> queryFileIdentityFromHandle(void* rawHandle)
{
    return queryFileIdentityFromHandleImpl(rawHandle, true);
}

std::optional<FileIdentity> queryFileIdentityFromHandleLightweight(void* rawHandle)
{
    return queryFileIdentityFromHandleImpl(rawHandle, false);
}

std::optional<FileIdentity> queryFileIdentity(const std::wstring& path)
{
    if (path.empty()) {
        return std::nullopt;
    }

    HANDLE h = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    const auto result = queryFileIdentityFromHandle(h);
    ::CloseHandle(h);
    return result;
}

std::wstring pathFromFileId(void* volumeHandle, std::uint64_t fileReferenceNumber)
{
    if (volumeHandle == nullptr ||
        volumeHandle == INVALID_HANDLE_VALUE || fileReferenceNumber == 0) {
        return {};
    }

    FILE_ID_DESCRIPTOR desc{};
    desc.dwSize = sizeof(desc);
    desc.Type = FileIdType;
    desc.FileId.QuadPart = static_cast<LONGLONG>(fileReferenceNumber);

    HANDLE h = ::OpenFileById(
        static_cast<HANDLE>(volumeHandle), &desc, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }

    // First probe size.
    DWORD needed = ::GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED);
    if (needed == 0) {
        ::CloseHandle(h);
        return {};
    }
    std::wstring buf(needed + 1, L'\0');
    const DWORD n =
        ::GetFinalPathNameByHandleW(h, buf.data(), needed + 1, FILE_NAME_NORMALIZED);
    ::CloseHandle(h);
    if (n == 0 || n > needed) {
        return {};
    }
    buf.resize(n);
    return canonicalWin32Path(buf);
}

bool pathIsUnderRoot(const std::wstring& path, const std::wstring& root)
{
    if (path.empty() || root.empty()) {
        return false;
    }
    // GetFinalPathNameByHandle returns long names. Hosted TEMP and some
    // user-supplied roots are 8.3 (C:\Users\RUNNER~1\...). Lexical prefix
    // compare without expansion treats in-root USN records as outside.
    const std::wstring p = toLowerCopy(canonicalWin32Path(path));
    const std::wstring r = toLowerCopy(canonicalWin32Path(root));
    if (p == r) {
        return true;
    }
    // Root with trailing separator match: "d:\proj" vs "d:\proj\foo"
    if (p.size() > r.size()) {
        if (p.compare(0, r.size(), r) != 0) {
            return false;
        }
        // Avoid "d:\project" matching root "d:\proj"
        if (r.back() == L'\\' || r.back() == L'/') {
            return true;
        }
        return p[r.size()] == L'\\' || p[r.size()] == L'/';
    }
    return false;
}

}  // namespace spacelens
