#pragma once

#include "core/PhysicalStorage.hpp"
#include "core/Types.hpp"
#include "core/ZaloContentIdentifier.hpp"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

/// Inspection/discovery status. ConfigUnavailable is non-fatal when usable
/// explicit or exact default roots were found, but records that configuration
/// discovery was not authoritative.
enum class ZaloStorageStatus {
    Complete,
    Partial,
    Cancelled,
    ConfigUnavailable,
    NoRoots,
    InvalidRoot,
    AccessDenied,
    Error
};

[[nodiscard]] const char* toString(ZaloStorageStatus status) noexcept;

enum class ZaloEntryKind {
    File,
    Directory,
    ReparseFile,
    ReparseDirectory
};

[[nodiscard]] const char* toString(ZaloEntryKind kind) noexcept;

/// Consistency is evidence quality, not deletion advice. Changed and
/// inconsistent observations are excluded from exact accounting totals.
enum class ZaloEntryConsistency {
    Consistent,
    Unknown,
    Inconsistent,
    Changed
};

[[nodiscard]] const char* toString(ZaloEntryConsistency consistency) noexcept;

using ZaloEvidenceConsistency = ZaloEntryConsistency;

using ZaloConfigKeyPath = std::vector<std::string>;

inline constexpr std::size_t kZaloMaxConfigBytes = 1024U * 1024U;

/// Options for exact-root discovery and read-only inspection. Explicit roots
/// may be a media root, an account root, or the exact ZaloDownloads directory.
/// No profile or drive recursion is performed.
struct ZaloDiscoveryOptions {
    std::vector<std::wstring> explicitRoots;
    bool includeDefaultRoots = true;

    /// Explicit comparison scope for `app-storage zalo items`. These paths
    /// are never discovered implicitly and are read-only enumerated without
    /// following reparse points.
    std::vector<std::wstring> comparisonPaths;

    /// Test/integration overrides for known-folder parents. Empty values use
    /// live Windows known folders and are never recursively enumerated.
    std::wstring roamingAppDataRoot;
    std::wstring localAppDataRoot;  // retained for source compatibility; unused
    std::wstring documentsRoot;

    /// An exact configuration file may be supplied by a caller. Production
    /// key paths are intentionally empty until externally verified. Non-empty
    /// paths are test/integration allowlist overrides, not a string scanner.
    std::wstring configPath;
    std::vector<ZaloConfigKeyPath> configJsonKeyPaths;
    /// Dotted key paths are accepted as a convenience for test callers. Each
    /// component is still matched exactly; values are never searched broadly.
    std::vector<std::string> configKeyPaths;

    /// Discover only the bounded media/account/ZaloDownloads shapes. This is
    /// retained as a compatibility switch; false only disables media-root
    /// account expansion and never enables generic-directory discovery.
    bool discoverAccounts = true;

    /// A caller may put its stop token here; overloads also accept one
    /// explicitly. Cancellation returns a typed Cancelled result.
    std::stop_token stopToken{};
};

using ZaloInspectionOptions = ZaloDiscoveryOptions;

struct ZaloRootSummary {
    std::string rootAlias;
    std::vector<std::string> accountAliases;
};

struct ZaloDiscoveryReport {
    ZaloStorageStatus status = ZaloStorageStatus::NoRoots;
    std::string detail;
    std::vector<ZaloRootSummary> roots;
    std::uint64_t rejectedRootCount = 0;

    [[nodiscard]] bool hasUsableRoots() const noexcept { return !roots.empty(); }
    [[nodiscard]] bool cancelled() const noexcept
    {
        return status == ZaloStorageStatus::Cancelled;
    }
};

/// One report-local entry. Native locators stay private to the inspection
/// session; callers identify entries only through the report-local ID.
struct ZaloEntry {
    std::string entryId;
    std::string categoryAlias;
    ZaloEntryKind kind = ZaloEntryKind::File;
    ByteSize logicalBytes = 0;
    std::optional<ByteSize> allocatedBytes;
    bool allocationKnown = false;
    bool reparsePoint = false;
    bool contentSkipped = false;
    std::optional<ZaloContentResult> contentIdentification;
    std::optional<ZaloHumanIdentity> humanIdentity;
    std::uint64_t lastWriteTicks = 0;
    bool identityKnown = false;
    bool hardLinkAlias = false;
    std::uint32_t filesystemLinkCount = 0;
    std::uint32_t observedPathCount = 0;
    std::optional<ByteSize> singlePathReleaseBytes;
    std::optional<ByteSize> allObservedPathReleaseBytes;
    ZaloEntryConsistency consistency = ZaloEntryConsistency::Unknown;
    std::wstring nativePath;
};

enum class ZaloExactCopyStatus {
    Disabled,
    Complete,
    Partial,
    Cancelled
};

[[nodiscard]] const char* toString(ZaloExactCopyStatus status) noexcept;

/// A privacy-safe, report-local proof that one consistent Zalo entry has the
/// same bytes as one explicitly scoped comparison file. Native paths and
/// digest values never leave the inspection process.
struct ZaloExactCopyMatch {
    std::string zaloEntryId;
    std::string comparisonFileId;
    std::string comparisonScopeAlias;
    std::string proofMethod = "full_sha256";
    std::string payloadKind;
    ByteSize matchedBytes = 0;
    std::uint64_t zaloHardLinkAliasCount = 0;
    std::uint64_t comparisonHardLinkAliasCount = 0;
};

struct ZaloExactCopyReport {
    bool enabled = false;
    ZaloExactCopyStatus status = ZaloExactCopyStatus::Disabled;
    std::string detail;
    std::string proofMethod = "full_sha256";
    std::uint64_t comparisonPathsRequested = 0;
    std::uint64_t comparisonFilesEnumerated = 0;
    std::uint64_t comparisonFilesHashed = 0;
    std::uint64_t uniqueComparisonFiles = 0;
    std::uint64_t comparisonHardLinkAliasesCollapsed = 0;
    std::uint64_t comparisonOverlappingPathsCollapsed = 0;
    std::uint64_t zaloEntriesConsidered = 0;
    std::uint64_t zaloHardLinkAliasesCollapsed = 0;
    std::uint64_t skippedReparsePoints = 0;
    std::uint64_t skippedInaccessible = 0;
    std::uint64_t skippedUnstable = 0;
    std::uint64_t skippedOversized = 0;
    std::uint64_t skippedUnsupported = 0;
    std::uint64_t skippedSameIdentity = 0;
    std::vector<std::string> comparisonScopeAliases;
    std::vector<ZaloExactCopyMatch> matches;

    [[nodiscard]] std::string toJson() const;
};

/// Accounting is deliberately split by meaning. Allocation remains optional;
/// logical size is never substituted when allocation is unavailable. Values
/// named `partialKnown*` are useful subtotals, never exact totals.
struct ZaloAccountingSummary {
    ByteSize pathVisibleLogicalBytes = 0;
    ByteSize uniqueLogicalBytes = 0;
    ByteSize partialKnownUniqueLogicalBytes = 0;
    std::optional<ByteSize> uniqueAllocatedBytes;
    std::optional<ByteSize> partialKnownUniqueAllocatedBytes;
    std::optional<ByteSize> allObservedPathReleaseBytes;
    ByteSize partialKnownReleaseBytes = 0;
    ByteSize hardLinkAliasBytes = 0;

    bool pathVisibleLogicalKnown = true;
    bool uniqueLogicalKnown = true;
    bool allocationKnown = true;
    bool hardLinkAliasKnown = true;
    bool logicalOverflow = false;
    bool allocationOverflow = false;
    bool releaseOverflow = false;

    HardLinkCoverage hardLinkCoverage = HardLinkCoverage::Unknown;
    std::uint64_t pathCount = 0;
    std::uint64_t uniqueIdentityCount = 0;
    std::uint64_t hardLinkAliasPathCount = 0;
    std::uint64_t unknownIdentityCount = 0;
    std::uint64_t unknownAllocationCount = 0;
    std::uint64_t inconsistentEvidenceCount = 0;
    std::uint64_t changedEvidenceCount = 0;
    std::uint64_t unknownLogicalCount = 0;
};

struct ZaloAccountReport {
    std::string rootAlias;
    std::string accountAlias;
    std::vector<ZaloEntry> entries;
    ZaloAccountingSummary accounting;
    std::uint64_t directoriesVisited = 0;
    std::uint64_t reparsePointsSkipped = 0;
    std::uint64_t accessDenied = 0;
    std::uint64_t otherErrors = 0;
    std::uint64_t unsafeEntriesSkipped = 0;
    bool complete = true;
};

struct ZaloStorageReport {
    ZaloStorageStatus status = ZaloStorageStatus::NoRoots;
    std::string detail;
    ZaloDiscoveryReport discovery;
    std::vector<ZaloRootSummary> roots;
    std::vector<ZaloAccountReport> accounts;
    ZaloAccountingSummary accounting;
    ZaloExactCopyReport exactCopy;

    [[nodiscard]] bool hasUsableRoots() const noexcept
    {
        return !roots.empty();
    }
    [[nodiscard]] bool cancelled() const noexcept
    {
        return status == ZaloStorageStatus::Cancelled;
    }
};

/// Discover only explicit roots and exact documented/default candidates. No
/// profile or drive recursion is performed, and no configuration keys are
/// inferred when the production allowlist is empty.
[[nodiscard]] ZaloDiscoveryReport discoverZaloRoots(
    const ZaloDiscoveryOptions& options = {});
[[nodiscard]] ZaloDiscoveryReport discoverZaloRoots(
    const ZaloDiscoveryOptions& options, std::stop_token stop);

/// Read-only inspection of discovered roots/accounts. After enumeration, bounded
/// payload bytes may be read once per strongly identified physical regular file
/// for privacy-safe content recognition; only an exact caller-allowlisted config
/// file may be read during discovery. Reparse points are opened no-follow.
[[nodiscard]] ZaloStorageReport inspectZaloStorage(
    const ZaloInspectionOptions& options = {});
[[nodiscard]] ZaloStorageReport inspectZaloStorage(
    const ZaloInspectionOptions& options, std::stop_token stop);

/// Small facade for callers that prefer an object over the free functions.
class ZaloStorageInspector final {
public:
    [[nodiscard]] ZaloDiscoveryReport discover(
        const ZaloDiscoveryOptions& options = {}) const;
    [[nodiscard]] ZaloDiscoveryReport discover(
        const ZaloDiscoveryOptions& options, std::stop_token stop) const;
    [[nodiscard]] ZaloStorageReport inspect(
        const ZaloInspectionOptions& options = {}) const;
    [[nodiscard]] ZaloStorageReport inspect(
        const ZaloInspectionOptions& options, std::stop_token stop) const;
};

}  // namespace spacelens
