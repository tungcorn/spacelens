#pragma once

#include "core/Types.hpp"
#include "core/index/IndexCatalog.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/treemap/TreemapLayout.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spacelens {

/// Storage overview for the currently browsed indexed location.
///
/// Aggregation rules (no double-counting of recursive directory sizes):
/// - `locationLogicalBytes` is the single authoritative total for this location:
///   root → published roots.logical_bytes; subdirectory → that entry's recursive_size.
/// - Direct child directories use recursive_size; direct files use size_bytes.
///   Their sum approximates `locationLogicalBytes` (subject to incomplete scans).
/// - Intelligence metrics are **counts of matching items**, never a sum of
///   overlapping recursive directory sizes labeled as "reclaimable bytes".
struct StorageOverview {
    std::wstring locationPath;
    bool isRoot = true;
    ByteSize locationLogicalBytes = 0;
    std::uint64_t directFileCount = 0;
    std::uint64_t directDirCount = 0;
    ByteSize directChildrenLogicalBytes = 0;  // sum of child effective sizes
    std::wstring largestChildName;
    std::wstring largestChildPath;
    ByteSize largestChildBytes = 0;
    IndexEntryKind largestChildKind = IndexEntryKind::File;
    bool hasLargestChild = false;

    /// Counts among direct children only (non-overlapping set).
    std::uint64_t developerCandidateCount = 0;
    std::uint64_t strongReclaimCount = 0;
    std::uint64_t moderateReclaimCount = 0;
    std::uint64_t oldAndLargeCount = 0;  // uses kOldAndLarge* thresholds

    std::uint64_t snapshotAgeMs = 0;
    std::string indexedAtIso;
    std::string freshnessLabel;
};

/// Hierarchy payload for treemap + overview (immediate children only).
struct HierarchyChildrenResult {
    bool ok = false;
    std::string error;
    std::wstring locationPath;
    ByteSize locationLogicalBytes = 0;
    std::vector<IndexHit> children;
    StorageOverview overview{};
    std::uint64_t query_elapsed_ms = 0;
};

/// Query immediate children of `locationPath` (or root when empty / equal root).
/// Uses persistent index only — no live filesystem walk.
/// `childLimit` caps SQL rows (default high enough for treemap + Other).
[[nodiscard]] HierarchyChildrenResult queryHierarchyChildren(
    const std::wstring& rootPath, const std::wstring& locationPath,
    std::size_t childLimit = 10000);

/// Build overview from already-fetched children + root summary metadata.
/// Pure logic — unit-tested without SQL when children are provided.
[[nodiscard]] StorageOverview buildStorageOverview(
    const IndexRootSummary& root, const std::wstring& locationPath,
    ByteSize locationLogicalBytes, const std::vector<IndexHit>& children,
    FileTimeTicks nowTicks = 0);

/// Convert hierarchy children into treemap weight items (path as key).
[[nodiscard]] std::vector<TreemapWeightItem> hierarchyToTreemapWeights(
    const std::vector<IndexHit>& children);

/// Look up a single indexed entry by absolute path (for location size).
[[nodiscard]] std::optional<IndexHit> lookupIndexEntry(
    const std::wstring& rootPath, const std::wstring& absolutePath);

/// True if key is the visualization-only Other bucket.
[[nodiscard]] bool isTreemapOtherKey(const std::wstring& key) noexcept;

/// Canonical Other key used by prepareTreemapWeights.
[[nodiscard]] const wchar_t* treemapOtherKey() noexcept;

}  // namespace spacelens
