#include "platform/windows/FileIdentity.hpp"

#include "core/SafetyPolicy.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cwctype>
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

    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = ::GetFileInformationByHandle(h, &info);
    ::CloseHandle(h);
    if (!ok) {
        return std::nullopt;
    }

    FileIdentity id;
    ULARGE_INTEGER idx;
    idx.HighPart = info.nFileIndexHigh;
    idx.LowPart = info.nFileIndexLow;
    id.fileId = idx.QuadPart;
    id.volumeSerial = info.dwVolumeSerialNumber;
    id.isDirectory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ULARGE_INTEGER sz;
    sz.HighPart = info.nFileSizeHigh;
    sz.LowPart = info.nFileSizeLow;
    id.sizeBytes = sz.QuadPart;
    id.lastWriteTicks = fileTimeToU64(info.ftLastWriteTime);
    id.lastAccessTicks = fileTimeToU64(info.ftLastAccessTime);
    id.attributes = info.dwFileAttributes;
    return id;
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
