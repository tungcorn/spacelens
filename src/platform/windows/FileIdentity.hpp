#pragma once

#include "core/Types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace spacelens {

/// NTFS file reference number (64-bit, from BY_HANDLE_FILE_INFORMATION).
/// For regular files, allocatedBytes is the bounded FileStreamInfo sum across
/// the unnamed and named data streams. It is unset when complete enumeration
/// cannot be proved; callers must never substitute logical sizeBytes. Directory
/// allocation remains the same-handle FILE_STANDARD_INFO value.
struct FileIdentity {
    /// The 64-bit reference is a fallback only. Prefer fileId128 when known.
    std::uint64_t fileId = 0;
    std::uint64_t volumeSerial = 0;
    std::array<std::uint8_t, 16> fileId128{};
    bool fileId128Known = false;
    bool fileIndex64Known = false;
    bool identityKnown = false;
    bool isDirectory = false;
    ByteSize sizeBytes = 0;
    bool logicalSizeKnown = false;
    std::optional<ByteSize> allocatedBytes;
    bool allocationKnown = false;
    std::uint32_t numberOfLinks = 0;
    bool linkCountKnown = false;
    bool sparse = false;
    bool compressed = false;
    std::uint64_t creationTimeTicks = 0;
    std::uint64_t changeTimeTicks = 0;
    std::uint64_t lastWriteTicks = 0;
    std::uint64_t lastAccessTicks = 0;
    bool basicMetadataKnown = false;
    std::uint32_t attributes = 0;
    bool observationConsistent = true;
};

/// Read identity, logical size, allocation, link count, attributes, and
/// timestamps from one already-open no-follow handle. The handle remains owned
/// by the caller. Returns nullopt when the handle cannot provide the basic
/// identity observation.
[[nodiscard]] std::optional<FileIdentity> queryFileIdentityFromHandle(
    void* handle);

/// Read only same-handle identity and stable metadata without enumerating named
/// streams or allocating a stream-information buffer. Regular-file allocation
/// is intentionally left unknown; callers must use queryFileIdentityFromHandle
/// when complete physical allocation evidence is required.
[[nodiscard]] std::optional<FileIdentity>
queryFileIdentityFromHandleLightweight(void* handle);

/// Open a path read-only (backup semantics for directories) and read all
/// identity/accounting evidence from that one no-follow handle. Returns nullopt
/// on access denied / not found / failure.
[[nodiscard]] std::optional<FileIdentity> queryFileIdentity(
    const std::wstring& path);

/// Resolve a full path for a file reference number on an open volume handle.
/// volumeHandle must be a volume opened with GENERIC_READ.
/// Returns empty string on failure (deleted, outside mount, access denied).
/// The result is a canonical Win32 path (long names, no \\?\ prefix).
[[nodiscard]] std::wstring pathFromFileId(void* volumeHandle,
                                          std::uint64_t fileReferenceNumber);

/// Expand 8.3 components and strip \\?\ so lexical comparison matches
/// GetFinalPathNameByHandle. Falls back to GetFullPathName + policy
/// normalize when the path does not exist (GetLongPathName requires it).
[[nodiscard]] std::wstring canonicalWin32Path(std::wstring_view path);

/// True if both sides resolve to the same location after 8.3 expansion.
[[nodiscard]] bool win32PathsEqual(std::wstring_view a, std::wstring_view b);

/// If `path` is under `root` after 8.3 expansion, return `root`'s lexical
/// form plus the relative suffix. Otherwise return `path` unchanged (policy
/// normalized). Keeps incremental USN rows in the same spelling as a full
/// walk of `root` — hosted TEMP is 8.3; GetFinalPathName is long.
[[nodiscard]] std::wstring rebasePathOntoRoot(const std::wstring& path,
                                              const std::wstring& root);

/// True if `path` is equal to or under `root` (case-insensitive, 8.3-aware).
[[nodiscard]] bool pathIsUnderRoot(const std::wstring& path,
                                   const std::wstring& root);

}  // namespace spacelens
