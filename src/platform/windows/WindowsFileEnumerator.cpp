#include "platform/windows/WindowsFileEnumerator.hpp"

#include "platform/windows/FindHandle.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace spacelens {
namespace {

std::uint64_t fileTimeToU64(const FILETIME& ft)
{
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

ByteSize fileSizeFromFindData(const WIN32_FIND_DATAW& data)
{
    ULARGE_INTEGER value;
    value.HighPart = data.nFileSizeHigh;
    value.LowPart = data.nFileSizeLow;
    return value.QuadPart;
}

bool isDotOrDotDot(const wchar_t* name)
{
    return name[0] == L'.' &&
           (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

EnumerateStatus mapFindError(DWORD error)
{
    switch (error) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return EnumerateStatus::AccessDenied;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return EnumerateStatus::NotFound;
    default:
        return EnumerateStatus::Error;
    }
}

}  // namespace

EnumerateResult WindowsFileEnumerator::enumerate(const std::wstring& directoryPath)
{
    EnumerateResult result;

    if (directoryPath.empty()) {
        result.status = EnumerateStatus::Error;
        result.message = L"Empty directory path";
        return result;
    }

    std::wstring pattern = directoryPath;
    if (pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern.push_back(L'\\');
    }
    pattern.append(L"*");

    WIN32_FIND_DATAW data{};
    // FIND_FIRST_EX_LARGE_FETCH reduces round-trips on large directories (Win7+).
    win::FindHandle handle(::FindFirstFileExW(
        pattern.c_str(),
        FindExInfoBasic,  // skip short names
        &data,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH));

    if (!handle) {
        result.status = mapFindError(::GetLastError());
        return result;
    }

    do {
        if (isDotOrDotDot(data.cFileName)) {
            continue;
        }

        EnumeratedEntry entry;
        entry.name = data.cFileName;
        entry.attributes = data.dwFileAttributes;
        entry.lastWriteTime = fileTimeToU64(data.ftLastWriteTime);

        const bool isDirectory =
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool isReparse =
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        if (isDirectory) {
            if (isReparse) {
                // Junctions / directory symlinks: do not recurse by default.
                entry.kind = EntryKind::ReparseDirectory;
                entry.size = 0;
            } else {
                entry.kind = EntryKind::Directory;
                entry.size = 0;
            }
        } else {
            // Files (including file symlinks) contribute their logical size.
            entry.kind = EntryKind::File;
            entry.size = fileSizeFromFindData(data);
        }

        result.entries.push_back(std::move(entry));
    } while (::FindNextFileW(handle.get(), &data));

    const DWORD endError = ::GetLastError();
    if (endError != ERROR_NO_MORE_FILES) {
        // Partial listing: keep what we have, flag an error count upstream.
        result.status = mapFindError(endError);
        if (result.status == EnumerateStatus::Ok) {
            result.status = EnumerateStatus::Error;
        }
        // Still return collected entries for partial survival.
    }

    return result;
}

}  // namespace spacelens
