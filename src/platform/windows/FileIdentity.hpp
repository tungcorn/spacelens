#pragma once

#include "core/Types.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace spacelens {

/// NTFS file reference number (64-bit, from BY_HANDLE_FILE_INFORMATION).
struct FileIdentity {
    std::uint64_t fileId = 0;
    std::uint64_t volumeSerial = 0;
    bool isDirectory = false;
    ByteSize sizeBytes = 0;
    std::uint64_t lastWriteTicks = 0;
    std::uint64_t lastAccessTicks = 0;
    std::uint32_t attributes = 0;
};

/// Open path read-only (backup semantics for directories) and read identity + basic attrs.
/// Returns nullopt on access denied / not found / failure.
[[nodiscard]] std::optional<FileIdentity> queryFileIdentity(
    const std::wstring& path);

/// Resolve a full path for a file reference number on an open volume handle.
/// volumeHandle must be a volume opened with GENERIC_READ.
/// Returns empty string on failure (deleted, outside mount, access denied).
[[nodiscard]] std::wstring pathFromFileId(void* volumeHandle,
                                          std::uint64_t fileReferenceNumber);

/// True if `path` is equal to or under `root` (case-insensitive, policy-normalized).
[[nodiscard]] bool pathIsUnderRoot(const std::wstring& path,
                                   const std::wstring& root);

}  // namespace spacelens
