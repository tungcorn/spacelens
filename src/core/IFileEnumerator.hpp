#pragma once

#include "core/Types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace spacelens {

enum class EntryKind {
    File,
    Directory,
    ReparseDirectory,  // junction/symlink dir — do not recurse by default
    Other
};

struct EnumeratedEntry {
    std::wstring name;  // leaf name only
    EntryKind kind = EntryKind::Other;
    ByteSize size = 0;                // logical size for files
    std::uint64_t lastWriteTime = 0;  // FILETIME-compatible 64-bit value
    std::uint64_t lastAccessTime = 0; // FILETIME-compatible 64-bit value
    std::uint32_t attributes = 0;
};

enum class EnumerateStatus {
    Ok,
    AccessDenied,
    NotFound,
    Cancelled,
    Error
};

struct EnumerateResult {
    EnumerateStatus status = EnumerateStatus::Ok;
    std::vector<EnumeratedEntry> entries;
    std::wstring message;  // optional diagnostic
};

/// Platform-neutral directory listing. Implementations must not follow
/// directory reparse points as if they were ordinary directories.
class IFileEnumerator {
public:
    virtual ~IFileEnumerator() = default;

    /// List the immediate children of `directoryPath` (absolute path).
    [[nodiscard]] virtual EnumerateResult enumerate(
        const std::wstring& directoryPath) = 0;
};

}  // namespace spacelens
