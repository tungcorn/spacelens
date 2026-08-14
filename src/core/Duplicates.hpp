#pragma once

#include "core/CleanupReview.hpp"
#include "core/FileTime.hpp"
#include "core/Types.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexStore.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

inline constexpr int kDuplicateSchemaVersion = 1;
inline constexpr ByteSize kDefaultDuplicateMinSize = 1024ULL * 1024ULL;
inline constexpr ByteSize kDuplicateSampleChunkBytes = 64ULL * 1024ULL;
inline constexpr ByteSize kDuplicateSampleThresholdBytes =
    3ULL * kDuplicateSampleChunkBytes;
inline constexpr std::size_t kDuplicateReadBufferBytes = 1024U * 1024U;

enum class DuplicateFileStatus {
    Candidate,
    Verified,
    SameIdentity,
    Missing,
    AccessDenied,
    ReparsePoint,
    NotRegularFile,
    SizeChanged,
    ChangedDuringRead,
    IdentityChanged,
    ReadError,
    Cancelled,
    Unsupported,
    EmptyIgnored
};

[[nodiscard]] const char* toString(DuplicateFileStatus status) noexcept;

struct DuplicateScanOptions {
    ByteSize minimumSize = kDefaultDuplicateMinSize;
    std::wstring userProfilePath;
    std::string generatedAt;
    /// Empty disables the persistent hash cache (tests). Product callers set
    /// spaceLensHashCachePath(). Path is never a cache key.
    std::wstring hashCachePath;
};

struct DuplicateIndexCandidate {
    std::wstring path;
    std::wstring name;
    ByteSize logicalSize = 0;
    FileTimeTicks lastWriteTicks = 0;
    std::uint32_t attributes = 0;
    bool indexedAsReparse = false;
};

struct DuplicateSizeBucket {
    ByteSize logicalSize = 0;
    std::vector<DuplicateIndexCandidate> files;
};

struct DuplicateCandidateQueryResult {
    bool ok = false;
    std::string error;
    IndexRootInfo root{};
    IndexLocation location{};
    std::uint64_t ageMs = 0;
    ByteSize minimumSize = 0;
    std::vector<DuplicateSizeBucket> buckets;
    std::uint64_t candidateFiles = 0;
    ByteSize candidateBytes = 0;
};

struct DuplicatePathRecord {
    std::wstring path;
    DuplicateFileStatus status = DuplicateFileStatus::Candidate;
    FileTimeTicks lastWrite = 0;
    std::uint32_t attributes = 0;
    bool hardLinkAlias = false;
};

struct DuplicateContentInstance {
    CleanupIdentity identity{};
    std::vector<DuplicatePathRecord> paths;
    ByteSize logicalSize = 0;
};

struct DuplicateGroup {
    std::string contentSha256Hex;
    std::string verification;
    ByteSize logicalSize = 0;
    std::vector<DuplicateContentInstance> instances;
    std::size_t distinctIdentityCount = 0;
    std::size_t pathCount = 0;
    std::size_t hardLinkAliasPathCount = 0;
    std::size_t redundantCopyCount = 0;
    ByteSize potentialRedundantLogicalBytes = 0;
    bool redundantBytesSaturated = false;
    FileTimeTicks verifiedAt = 0;
};

struct DuplicateSkip {
    std::wstring path;
    ByteSize indexedSize = 0;
    DuplicateFileStatus status = DuplicateFileStatus::ReadError;
    std::string detail;
};

struct DuplicateScanProgress {
    std::uint64_t candidateFiles = 0;
    ByteSize candidateBytes = 0;
    std::uint64_t filesProbed = 0;
    std::uint64_t filesFingerprinted = 0;
    std::uint64_t filesFullyHashed = 0;
    ByteSize bytesRead = 0;
    ByteSize bytesFullyHashed = 0;
    ByteSize bytesReusedFromCache = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t cacheInvalidations = 0;
    std::uint64_t cacheWrites = 0;
    std::uint64_t verifiedGroups = 0;
    std::uint64_t skippedFiles = 0;
};

struct DuplicateDetectionSummary {
    std::uint64_t verifiedGroups = 0;
    std::uint64_t verifiedPaths = 0;
    std::uint64_t distinctContentInstances = 0;
    std::uint64_t hardLinkAliasPaths = 0;
    ByteSize potentialRedundantLogicalBytes = 0;
    bool redundantBytesSaturated = false;
    ByteSize bytesRead = 0;
    ByteSize bytesFullyHashed = 0;
    ByteSize bytesReusedFromCache = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t cacheInvalidations = 0;
    std::uint64_t cacheWrites = 0;
    std::uint64_t filesFullyHashed = 0;
    std::uint64_t candidateFiles = 0;
    ByteSize candidateBytes = 0;
    std::uint64_t skippedFiles = 0;
};

struct DuplicateDetectionResult {
    int schemaVersion = kDuplicateSchemaVersion;
    std::string source = "persistent_index";
    std::string verification = "full_sha256";
    bool planningOnly = true;
    bool readOnly = true;
    bool filesystemMutation = false;
    std::wstring root;
    std::string generatedAt = "1970-01-01T00:00:00Z";
    ByteSize minimumSize = kDefaultDuplicateMinSize;
    bool completed = false;
    bool cancelled = false;
    std::string error;
    std::uint64_t indexAgeMs = 0;
    std::string indexIndexedAtIso;
    DuplicateScanProgress progress{};
    DuplicateDetectionSummary summary{};
    std::vector<DuplicateGroup> groups;
    std::vector<DuplicateSkip> skipped;

    [[nodiscard]] std::string toText(const DuplicateScanOptions& options = {}) const;
    [[nodiscard]] std::string toJson(const DuplicateScanOptions& options = {}) const;
};

[[nodiscard]] ByteSize saturatingMul(ByteSize a, ByteSize b, bool& saturated) noexcept;
[[nodiscard]] ByteSize redundantLogicalBytes(std::size_t distinctInstances,
                                             ByteSize logicalSize,
                                             bool& saturated) noexcept;
[[nodiscard]] ByteSize redundantLogicalBytes(std::size_t distinctInstances,
                                             ByteSize logicalSize) noexcept;

void finalizeDuplicateGroup(DuplicateGroup& group) noexcept;
void sortDuplicateGroups(std::vector<DuplicateGroup>& groups);
void accumulateDuplicateSummary(DuplicateDetectionResult& result) noexcept;

[[nodiscard]] std::string sha256ToHex(const std::array<std::uint8_t, 32>& digest);
[[nodiscard]] std::string duplicateIdentityKey(const CleanupIdentity& identity);

[[nodiscard]] CleanupCandidate cleanupCandidateFromDuplicate(
    const DuplicateGroup& group,
    const DuplicateContentInstance& instance,
    const DuplicatePathRecord& path,
    const std::wstring& sourceRoot,
    std::uint64_t indexAgeMs,
    const std::string& indexedAtIso);

enum class ContentHashKind {
    Sample,
    Full
};

struct ContentHashRequest {
    std::wstring path;
    ByteSize expectedSize = 0;
    CleanupIdentity expectedIdentity{};
    FileTimeTicks expectedLastWrite = 0;
    ContentHashKind kind = ContentHashKind::Full;
    std::function<bool()> cancelled;
};

struct ContentHashResult {
    DuplicateFileStatus status = DuplicateFileStatus::ReadError;
    std::array<std::uint8_t, 32> digest{};
    ByteSize bytesRead = 0;
    CleanupIdentity identity{};
    ByteSize logicalSize = 0;
    FileTimeTicks lastWrite = 0;
    FileTimeTicks changeTime = 0;
    std::int64_t fileUsn = 0;
    std::uint64_t journalId = 0;
    std::uint32_t attributes = 0;
    std::uint32_t nativeError = 0;
    bool persistable = false;
    std::string detail;
};

/// Live evidence for cache lookup. Path is intentionally absent — never a key.
struct ContentHashEvidence {
    CleanupIdentity identity{};
    ByteSize logicalSize = 0;
    FileTimeTicks lastWrite = 0;
    FileTimeTicks changeTime = 0;
    std::int64_t fileUsn = 0;
    std::uint64_t journalId = 0;
    bool persistable = false;
    DuplicateFileStatus status = DuplicateFileStatus::Unsupported;
    std::string detail;
};

[[nodiscard]] inline ContentHashEvidence evidenceFrom(
    const ContentHashResult& result) noexcept
{
    ContentHashEvidence evidence;
    evidence.identity = result.identity;
    evidence.logicalSize = result.logicalSize;
    evidence.lastWrite = result.lastWrite;
    evidence.changeTime = result.changeTime;
    evidence.fileUsn = result.fileUsn;
    evidence.journalId = result.journalId;
    evidence.persistable = result.persistable;
    evidence.status = result.status;
    evidence.detail = result.detail;
    return evidence;
}

class IFileContentHasher {
public:
    virtual ~IFileContentHasher() = default;
    [[nodiscard]] virtual ContentHashResult hash(const ContentHashRequest& request) = 0;
    /// Default is non-persistable so fake hashers never open AppData.
    [[nodiscard]] virtual ContentHashEvidence probe(const std::wstring& path)
    {
        (void)path;
        return {};
    }
};

}  // namespace spacelens
