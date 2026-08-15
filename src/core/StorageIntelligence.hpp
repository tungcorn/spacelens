#pragma once

#include "core/Classification.hpp"
#include "core/DirectoryTree.hpp"
#include "core/ReclaimAnalysis.hpp"
#include "core/ScanTypes.hpp"
#include "core/Types.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/index/IndexSnapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spacelens {

inline constexpr int kStorageIntelligenceSchemaVersion = 1;
inline constexpr const char* kOpportunityRankPolicy = "opportunity_rank_v2";
inline constexpr std::size_t kDefaultOverviewLimit = 10;
inline constexpr std::size_t kDefaultOpportunityLimit = 20;
inline constexpr ByteSize kDefaultOpportunityMinSize = 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultOldLargeDays = 90;
/// Historical internal prefetch used before Indexed Intelligence Scaling V1.
/// Production indexed opportunities no longer fetch this prefix.
inline constexpr std::size_t kIndexedOpportunityFetchLimit = 200;
/// Historical materialization ceiling used before Exact Indexed Opportunity
/// Aggregates V1. Production no longer caps overlap accounting at this count.
inline constexpr std::size_t kIndexedOpportunityAggregateLimit = 50000;

/// Stable, non-localized reason codes derived from existing analysis semantics.
namespace reason {
inline constexpr const char* kLargeDirectory = "large_directory";
inline constexpr const char* kLargeFile = "large_file";
inline constexpr const char* kOldLargeFile = "old_large_file";
inline constexpr const char* kDeveloperDependency = "developer_dependency";
inline constexpr const char* kGeneratedOutput = "generated_output";
inline constexpr const char* kPackageCache = "package_cache";
inline constexpr const char* kIdeCache = "ide_cache";
inline constexpr const char* kTemporaryData = "temporary_data";
inline constexpr const char* kLogData = "log_data";
inline constexpr const char* kProtectedLocation = "protected_location";
inline constexpr const char* kSensitiveLocation = "sensitive_location";
inline constexpr const char* kUnknownLocation = "unknown_location";
inline constexpr const char* kRecentActivity = "recent_activity";
inline constexpr const char* kUserOrUnknownContent = "user_or_unknown_content";
inline constexpr const char* kLikelyRegenerable = "likely_regenerable";
inline constexpr const char* kPossiblyRegenerable = "possibly_regenerable";
inline constexpr const char* kStrongCandidate = "strong_candidate";
inline constexpr const char* kModerateCandidate = "moderate_candidate";
inline constexpr const char* kReviewOnly = "review_only";
inline constexpr const char* kKnownDependencyTree = "known_dependency_tree";
inline constexpr const char* kKnownGeneratedOutput = "known_generated_output";
inline constexpr const char* kKnownPackageCache = "known_package_cache";
inline constexpr const char* kKnownTempLocation = "known_temp_location";
inline constexpr const char* kOldLargeArchive = "old_large_archive";
inline constexpr const char* kOldLargeInstaller = "old_large_installer";
inline constexpr const char* kDownloadsLocation = "downloads_location";
inline constexpr const char* kNestedOverlap = "nested_overlap";
}  // namespace reason

enum class EvidenceSource {
    LiveScan,
    PersistentIndex
};

[[nodiscard]] const char* toString(EvidenceSource source) noexcept;

struct StorageConsumer {
    std::wstring path;
    std::string objectType;  // "file" | "directory"
    ByteSize logicalBytes = 0;
    std::string classification;
    std::string confidence;
    std::string reclaimability;
    std::string locationSafety;
    std::string candidateStrength;
    std::vector<std::string> reasonCodes;
};

struct OpportunityGroup {
    std::string id;
    std::string classification;
    ByteSize logicalBytes = 0;
    std::uint64_t itemCount = 0;
    bool estimated = false;
    std::string strongestCandidateStrength;
    std::vector<std::string> reasonCodes;
};

struct StorageOverviewReport {
    int schemaVersion = kStorageIntelligenceSchemaVersion;
    EvidenceSource source = EvidenceSource::LiveScan;
    std::wstring root;
    std::string state = "completed";
    bool ok = true;
    std::string error;
    ByteSize logicalBytes = 0;
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t accessDenied = 0;
    std::uint64_t reparseSkipped = 0;
    std::uint64_t otherErrors = 0;
    std::uint64_t elapsedMs = 0;
    std::vector<StorageConsumer> largestDirectories;
    std::vector<StorageConsumer> largestFiles;
    std::uint64_t returnedDirectories = 0;
    std::uint64_t returnedFiles = 0;
    bool truncatedDirectories = false;
    bool truncatedFiles = false;
    std::uint64_t indexAgeMs = 0;
    std::string indexedAtIso;
    IndexSnapshotEvidence snapshot{};
    IndexAgeDecision ageDecision{};
    /// Compact live-scan category totals. Empty for indexed overview (no extra
    /// regenerable fetch) so overview does not become a second opportunities dump.
    std::vector<OpportunityGroup> opportunitySummary;

    [[nodiscard]] std::string toJson() const;
};

struct OpportunityItem {
    std::wstring path;
    std::string objectType;
    ByteSize logicalBytes = 0;
    std::string classification;
    std::string confidence;
    std::string ruleId;
    std::string reclaimability;
    std::string candidateStrength;
    std::string locationSafety;
    std::optional<std::uint64_t> inactiveDays;
    FileTimeTicks activityWriteTicks = 0;
    std::vector<std::string> reasonCodes;
    std::string explanation;
    std::string ecosystem;
    std::string marker;
    int opportunityRank = 0;
    bool overlapped = false;
};

struct OpportunityQuery {
    ByteSize minSize = kDefaultOpportunityMinSize;
    std::uint64_t olderThanDays = kDefaultOldLargeDays;
    FileTimeTicks nowTicks = 0;
    std::size_t limit = kDefaultOpportunityLimit;
    std::optional<StorageCategory> categoryOnly;
    /// Unrecognized classification token: include nothing (do not treat as Unknown).
    bool matchNone = false;
    /// When non-empty, restrict to this path and descendants (normalized).
    std::wstring pathPrefix;
};

/// Slim row for overlap-aware streaming aggregation. Not a full Opportunity DTO.
struct OpportunityAggregateInput {
    std::wstring path;
    bool directory = false;
    ByteSize logicalBytes = 0;
    std::string classification;
    std::string candidateStrength;
    bool oldLargeFile = false;
};

/// Bounded path-order reducer: O(nesting depth + group count) memory.
/// Directory candidates cover descendants; files never do. Duplicate normalized
/// paths are ignored. Overflow saturates to UINT64_MAX and marks estimated.
class OpportunityStreamReducer {
public:
    void watchPath(std::wstring path);
    void observe(OpportunityAggregateInput row);

    [[nodiscard]] bool overflow() const noexcept { return overflow_; }
    [[nodiscard]] ByteSize uniqueReviewBytes() const noexcept
    {
        return uniqueReviewBytes_;
    }
    [[nodiscard]] bool uniqueReviewEstimated() const noexcept
    {
        return uniqueReviewEstimated_;
    }
    [[nodiscard]] std::uint64_t rowsStreamed() const noexcept
    {
        return rowsStreamed_;
    }
    [[nodiscard]] std::uint64_t contributingRows() const noexcept
    {
        return contributingRows_;
    }
    [[nodiscard]] std::uint64_t coveredRows() const noexcept
    {
        return coveredRows_;
    }
    [[nodiscard]] std::uint64_t duplicateRows() const noexcept
    {
        return duplicateRows_;
    }
    [[nodiscard]] std::size_t maxActiveDepth() const noexcept
    {
        return maxActiveDepth_;
    }
    [[nodiscard]] const std::vector<OpportunityGroup>& groups() const noexcept
    {
        return groups_;
    }
    [[nodiscard]] bool watchedPathOverlapped(const std::wstring& path) const;
    [[nodiscard]] std::vector<std::wstring> overlappedWatchedKeys() const;
    void finalizeGroups();

private:
    struct Watch {
        std::wstring key;
        bool overlapped = false;
        bool seen = false;
    };

    std::vector<std::wstring> stack_;
    std::wstring lastKey_;
    std::vector<Watch> watched_;
    std::vector<OpportunityGroup> groups_;
    ByteSize uniqueReviewBytes_ = 0;
    bool uniqueReviewEstimated_ = false;
    bool overflow_ = false;
    bool groupsFinalized_ = false;
    std::uint64_t rowsStreamed_ = 0;
    std::uint64_t contributingRows_ = 0;
    std::uint64_t coveredRows_ = 0;
    std::uint64_t duplicateRows_ = 0;
    std::size_t maxActiveDepth_ = 0;
};

/// Optional indexed-fetch metadata for exact top-N + streamed aggregates.
struct IndexedOpportunityExtras {
    const std::vector<IndexHit>* aggregateHits = nullptr;
    bool aggregatesCapped = false;
    std::uint64_t matchedCount = 0;
    bool hasStreamedAggregate = false;
    ByteSize uniqueReviewBytes = 0;
    bool uniqueReviewEstimated = false;
    bool aggregateOverflow = false;
    std::vector<OpportunityGroup> groups;
    std::vector<std::wstring> overlappedPathKeys;
    std::uint64_t rowsStreamed = 0;
    std::size_t maxActiveDepth = 0;
};

/// Case-folded component path used by overlap / --under. Drive root `D:\`
/// normalizes to `d:` so it is a prefix of `d:\users\...`.
[[nodiscard]] std::wstring normalizeOpportunityPathKey(std::wstring path);

struct OpportunityReport {
    int schemaVersion = kStorageIntelligenceSchemaVersion;
    EvidenceSource source = EvidenceSource::LiveScan;
    std::wstring root;
    std::string state = "completed";
    bool ok = true;
    std::string error;
    ByteSize logicalBytes = 0;
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    ByteSize uniqueReviewBytes = 0;
    bool uniqueReviewEstimated = false;
    std::uint64_t returnedCount = 0;
    bool truncated = false;
    std::uint64_t accessDenied = 0;
    std::uint64_t reparseSkipped = 0;
    std::uint64_t otherErrors = 0;
    std::uint64_t elapsedMs = 0;
    std::uint64_t indexAgeMs = 0;
    std::string indexedAtIso;
    IndexSnapshotEvidence snapshot{};
    IndexAgeDecision ageDecision{};
    std::string rankingPolicy = kOpportunityRankPolicy;
    std::vector<OpportunityGroup> groups;
    std::vector<OpportunityItem> opportunities;

    [[nodiscard]] std::string toJson() const;
};

/// Largest consumers from one live scan snapshot. Does not promote reclaim.
[[nodiscard]] StorageOverviewReport buildLiveOverview(
    const ScanResult& result, std::size_t limit = kDefaultOverviewLimit,
    FileTimeTicks nowTicks = 0);

/// Largest consumers from published-index query hits.
[[nodiscard]] StorageOverviewReport buildIndexedOverview(
    const std::wstring& root, ByteSize logicalBytes, std::uint64_t files,
    std::uint64_t directories, const std::vector<IndexHit>& directoryHits,
    const std::vector<IndexHit>& fileHits, std::uint64_t indexAgeMs,
    std::string indexedAtIso, std::size_t limit = kDefaultOverviewLimit);

/// Ranked review opportunities from a live DirectoryTree.
/// Nested directory candidates are listed but unique_review_bytes does not
/// double-count descendants covered by a selected ancestor directory.
[[nodiscard]] OpportunityReport buildLiveOpportunities(
    const DirectoryTree& tree, const OpportunityQuery& query);

/// Ranked review opportunities from already-fetched index hits.
[[nodiscard]] OpportunityReport buildIndexedOpportunities(
    const std::wstring& root, ByteSize logicalBytes, std::uint64_t files,
    std::uint64_t directories, const std::vector<IndexHit>& hits,
    const OpportunityQuery& query, std::uint64_t indexAgeMs,
    std::string indexedAtIso,
    IndexedOpportunityExtras extras = {});

/// Reason codes for an analyzed item. Pure and deterministic.
[[nodiscard]] std::vector<std::string> reasonCodesFor(
    const ReclaimCandidate& candidate, bool oldLargeFile);

[[nodiscard]] const char* objectTypeName(ItemKind kind) noexcept;

/// Group id for a classified opportunity, or empty if it should not form a
/// regenerable aggregate (old-large / unknown / user content).
[[nodiscard]] std::string opportunityGroupId(StorageCategory category);

/// Classifications that are typically regenerable and must be fetched for
/// indexed opportunities. Excludes DownloadedAiModel (not regenerable).
/// Includes TemporaryData (GUI developer-storage preset omits it).
[[nodiscard]] const std::vector<std::string>&
regenerableOpportunityClassifications();

}  // namespace spacelens
