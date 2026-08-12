#pragma once

#include "core/index/IndexPaths.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/index/IndexRefresh.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spacelens {

/// Agent/GUI-facing freshness of a published index (snapshot, not live FS truth).
enum class IndexFreshness {
    Missing,
    Fresh,                   // age below soft threshold
    AgedSnapshot,            // exists but older than soft threshold
    RefreshAvailable,        // USN incremental possible
    IncrementalUnavailable,  // index ok; USN cannot run (access, journal, …)
    FullRebuildRequired,     // discontinuity / needs full index
    Error
};

[[nodiscard]] const char* toString(IndexFreshness f) noexcept;

/// Soft age threshold for "Fresh" vs "Aged snapshot" (display only).
inline constexpr std::uint64_t kFreshAgeMs = 15ULL * 60ULL * 1000ULL;  // 15 min

struct IndexRootSummary {
    std::wstring rootPath;
    std::wstring rootKey;
    std::wstring dbPath;
    bool exists = false;
    IndexFreshness freshness = IndexFreshness::Missing;
    std::string freshnessLabel;  // human short label
    std::string reason;          // machine reason when not fresh
    std::uint64_t ageMs = 0;
    std::uint64_t fileCount = 0;
    std::uint64_t dirCount = 0;
    ByteSize logicalBytes = 0;
    std::string indexedAtIso;
    std::string fullIndexedAtIso;
    std::string lastRefreshMethod;  // full | usn
    std::string checkpointStatus;
    bool incrementalSupported = false;
    IncrementalRefreshState incrementalState = IncrementalRefreshState::Unknown;
};

/// First-class storage-discovery modes for Index Browser V2.
/// Each maps to a typed IndexQuerySpec (no ad-hoc SQL in the UI).
enum class IndexDiscoveryPreset {
    Custom = 0,           // user-driven filters only
    Largest,              // largest files/folders
    OldAndLarge,          // large + inactive by activity write age
    DeveloperStorage,     // build/deps/cache/model/log categories
    ReclaimCandidates     // strong/moderate reclaim strength
};

[[nodiscard]] const char* toString(IndexDiscoveryPreset p) noexcept;

/// Default size floor for Old & Large (100 MiB).
inline constexpr ByteSize kOldAndLargeMinBytes = 100ULL * 1024ULL * 1024ULL;
/// Default inactivity floor for Old & Large (90 days).
inline constexpr std::uint64_t kOldAndLargeOlderThanDays = 90;

/// Map probe + status into a single freshness enum (pure logic; unit-tested).
[[nodiscard]] IndexFreshness mapIndexFreshness(bool indexExists,
                                               std::uint64_t ageMs,
                                               IncrementalRefreshState incState,
                                               IndexRefreshOutcome probeOutcome);

/// Summarize one root (opens DB read-only + probe).
[[nodiscard]] IndexRootSummary summarizeIndexedRoot(const std::wstring& rootPath);

/// Enumerate published indexes and summarize each (best-effort).
[[nodiscard]] std::vector<IndexRootSummary> listIndexSummaries();

/// Build IndexQuerySpec from simple UI fields (shared CLI/GUI filter semantics).
[[nodiscard]] IndexQuerySpec makeBrowserQuerySpec(
    bool includeFiles, bool includeDirectories, std::optional<ByteSize> minSize,
    std::string extensionLowerNoDot, std::string classification,
    std::string candidateStrength, std::size_t limit);

/// Apply a discovery preset onto a base spec (limit / browse / search / extra
/// filters from the UI are preserved when already set; preset fills defaults
/// for kind, sort, and domain filters).
[[nodiscard]] IndexQuerySpec applyDiscoveryPreset(IndexDiscoveryPreset preset,
                                                  IndexQuerySpec base);

/// Build a complete discovery query from preset + common UI overrides.
[[nodiscard]] IndexQuerySpec makeDiscoveryQuery(
    IndexDiscoveryPreset preset, bool includeFiles, bool includeDirectories,
    std::optional<ByteSize> minSize, std::optional<std::uint64_t> olderThanDays,
    std::string extensionLowerNoDot, std::string classification,
    std::string candidateStrength, std::string searchText,
    std::wstring browsePath, IndexSortKey sortBy, bool sortDescending,
    std::size_t limit);

/// Developer-storage classification names used by the Developer Storage preset.
[[nodiscard]] const std::vector<std::string>& developerStorageClassifications();

}  // namespace spacelens
