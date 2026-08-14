#include "core/index/IndexPaths.hpp"

#include "core/SafetyPolicy.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>

#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace spacelens {
namespace {

std::wstring joinPath(std::wstring_view a, std::wstring_view b)
{
    if (a.empty()) {
        return std::wstring(b);
    }
    if (b.empty()) {
        return std::wstring(a);
    }
    std::wstring out(a);
    if (out.back() != L'\\' && out.back() != L'/') {
        out.push_back(L'\\');
    }
    if (!b.empty() && (b.front() == L'\\' || b.front() == L'/')) {
        out.append(b.substr(1));
    } else {
        out.append(b);
    }
    return out;
}

// FNV-1a 64-bit over UTF-16 code units — stable, no crypto dependency.
std::uint64_t fnv1a64(std::wstring_view text)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const wchar_t ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::wstring toHex(std::uint64_t value)
{
    std::wostringstream os;
    os << std::hex;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const int nibble = static_cast<int>((value >> shift) & 0xF);
        os << L"0123456789abcdef"[nibble];
    }
    return os.str();
}

}  // namespace

std::wstring spaceLensDataRoot()
{
    wchar_t overrideBuf[32768]{};
    const DWORD overrideLen =
        ::GetEnvironmentVariableW(L"SPACELENS_DATA_ROOT", overrideBuf, 32768);
    if (overrideLen > 0 && overrideLen < 32768) {
        return std::wstring(overrideBuf, overrideLen);
    }

    PWSTR path = nullptr;
    const HRESULT hr =
        ::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
    if (FAILED(hr) || path == nullptr) {
        // Fallback: %USERPROFILE%\AppData\Local
        wchar_t buffer[MAX_PATH]{};
        const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                  MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return L"SpaceLensData";
        }
        return joinPath(buffer, L"SpaceLens");
    }
    std::wstring root = joinPath(path, L"SpaceLens");
    ::CoTaskMemFree(path);
    return root;
}

std::wstring spaceLensIndexesRoot()
{
    return joinPath(spaceLensDataRoot(), L"indexes");
}

std::wstring spaceLensReviewStatePath()
{
    return joinPath(spaceLensDataRoot(), L"state.db");
}

std::wstring spaceLensHashCachePath()
{
    return joinPath(spaceLensDataRoot(), L"hash-cache.db");
}

std::wstring normalizeIndexRoot(std::wstring_view path)
{
    // Reuse policy normalizer (drive roots, trailing slash strip).
    return normalizePathForPolicy(path);
}

std::wstring rootKeyFor(std::wstring_view normalizedRoot)
{
    std::wstring lower;
    lower.reserve(normalizedRoot.size());
    for (wchar_t ch : normalizedRoot) {
        lower.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return toHex(fnv1a64(lower));
}

IndexLocation locateIndex(std::wstring_view rootPath)
{
    IndexLocation loc;
    loc.rootPath = normalizeIndexRoot(rootPath);
    loc.rootKey = rootKeyFor(loc.rootPath);
    loc.indexDir = joinPath(spaceLensIndexesRoot(), loc.rootKey);
    loc.dbPath = joinPath(loc.indexDir, L"index.db");
    loc.stagingDbPath = joinPath(loc.indexDir, L"index.db.building");
    return loc;
}

bool ensureDirectory(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    // Create parents recursively via SHCreateDirectoryExW.
    const int rc = ::SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS ||
           rc == ERROR_FILE_EXISTS;
}

bool publishIndexDatabase(const IndexLocation& loc)
{
    if (loc.dbPath.empty() || loc.stagingDbPath.empty()) {
        return false;
    }
    const DWORD stagingAttr = ::GetFileAttributesW(loc.stagingDbPath.c_str());
    if (stagingAttr == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    const std::wstring backup = loc.dbPath + L".bak";
    ::DeleteFileW(backup.c_str());

    const DWORD liveAttr = ::GetFileAttributesW(loc.dbPath.c_str());
    if (liveAttr != INVALID_FILE_ATTRIBUTES) {
        if (!::MoveFileW(loc.dbPath.c_str(), backup.c_str())) {
            return false;
        }
    }

    if (!::MoveFileExW(loc.stagingDbPath.c_str(), loc.dbPath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED |
                           MOVEFILE_WRITE_THROUGH)) {
        // Restore backup if we moved it aside.
        if (liveAttr != INVALID_FILE_ATTRIBUTES) {
            ::MoveFileW(backup.c_str(), loc.dbPath.c_str());
        }
        return false;
    }

    ::DeleteFileW(backup.c_str());
    return true;
}

void discardStagingDatabase(const IndexLocation& loc)
{
    if (!loc.stagingDbPath.empty()) {
        ::DeleteFileW(loc.stagingDbPath.c_str());
    }
}

bool indexDatabaseExists(const IndexLocation& loc)
{
    if (loc.dbPath.empty()) {
        return false;
    }
    const DWORD attr = ::GetFileAttributesW(loc.dbPath.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::vector<ListedIndex> listIndexedRoots()
{
    std::vector<ListedIndex> out;
    const std::wstring root = spaceLensIndexesRoot();
    const std::wstring pattern = joinPath(root, L"*");
    WIN32_FIND_DATAW fd{};
    const HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return out;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        if (fd.cFileName[0] == L'.') {
            continue;
        }
        ListedIndex item;
        item.rootKey = fd.cFileName;
        item.dbPath = joinPath(joinPath(root, fd.cFileName), L"index.db");
        const DWORD attr = ::GetFileAttributesW(item.dbPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES ||
            (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        // Root path is stored inside the DB; filled by callers that open it.
        out.push_back(std::move(item));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return out;
}

}  // namespace spacelens
