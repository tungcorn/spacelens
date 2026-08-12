#include "core/index/IndexOverview.hpp"

#include "core/FileTime.hpp"
#include "core/index/IndexStore.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>

namespace spacelens {
namespace {

constexpr const wchar_t* kOtherKey = L"__spacelens_treemap_other__";

FileTimeTicks nowFileTime()
{
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

std::wstring normalizeLocation(const std::wstring& rootPath,
                               const std::wstring& locationPath)
{
    if (locationPath.empty() || locationPath == rootPath) {
        return rootPath;
    }
    return locationPath;
}

bool isDeveloperClass(const std::string& classification)
{
    for (const auto& c : developerStorageClassifications()) {
        if (c == classification) {
            return true;
        }
    }
    return false;
}

bool isOldAndLarge(const IndexHit& h, FileTimeTicks now)
{
    if (h.size_bytes < kOldAndLargeMinBytes) {
        return false;
    }
    if (h.last_write_ticks == 0 || now == 0) {
        return false;
    }
    const FileTimeTicks cutoff =
        now > daysToTicks(kOldAndLargeOlderThanDays)
            ? now - daysToTicks(kOldAndLargeOlderThanDays)
            : 0;
    return h.last_write_ticks > 0 && h.last_write_ticks <= cutoff;
}

}  // namespace

const wchar_t* treemapOtherKey() noexcept
{
    return kOtherKey;
}

bool isTreemapOtherKey(const std::wstring& key) noexcept
{
    return key == kOtherKey;
}

std::optional<IndexHit> lookupIndexEntry(const std::wstring& rootPath,
                                         const std::wstring& absolutePath)
{
    if (absolutePath.empty()) {
        return std::nullopt;
    }
    IndexQuerySpec spec;
    spec.includeFiles = true;
    spec.includeDirectories = true;
    // pathPrefix with exact path: use pathPrefix = absolutePath and limit 1,
    // but pathPrefix matches path OR descendants. Prefer browse of parent + filter.
    // Direct SQL via path equality through pathPrefix + post-filter.
    spec.pathPrefix = absolutePath;
    spec.limit = 8;
    spec.sortBy = IndexSortKey::Size;
    auto r = queryIndex(rootPath, spec);
    if (!r.ok) {
        return std::nullopt;
    }
    for (const auto& h : r.hits) {
        if (h.path == absolutePath) {
            return h;
        }
    }
    return std::nullopt;
}

StorageOverview buildStorageOverview(const IndexRootSummary& root,
                                     const std::wstring& locationPath,
                                     ByteSize locationLogicalBytes,
                                     const std::vector<IndexHit>& children,
                                     FileTimeTicks nowTicks)
{
    StorageOverview o;
    o.locationPath = normalizeLocation(root.rootPath, locationPath);
    o.isRoot = (o.locationPath == root.rootPath || locationPath.empty());
    o.locationLogicalBytes = locationLogicalBytes;
    if (o.isRoot && o.locationLogicalBytes == 0) {
        o.locationLogicalBytes = root.logicalBytes;
    }
    o.snapshotAgeMs = root.ageMs;
    o.indexedAtIso = root.indexedAtIso;
    o.freshnessLabel = root.freshnessLabel;

    const FileTimeTicks now = nowTicks != 0 ? nowTicks : nowFileTime();

    ByteSize largest = 0;
    for (const auto& h : children) {
        if (h.kind == IndexEntryKind::Directory) {
            ++o.directDirCount;
        } else {
            ++o.directFileCount;
        }
        o.directChildrenLogicalBytes += h.size_bytes;
        if (!o.hasLargestChild || h.size_bytes > largest) {
            largest = h.size_bytes;
            o.hasLargestChild = true;
            o.largestChildBytes = h.size_bytes;
            o.largestChildName = h.name;
            o.largestChildPath = h.path;
            o.largestChildKind = h.kind;
        }
        if (isDeveloperClass(h.classification)) {
            ++o.developerCandidateCount;
        }
        if (h.candidate_strength == "Strong") {
            ++o.strongReclaimCount;
        } else if (h.candidate_strength == "Moderate") {
            ++o.moderateReclaimCount;
        }
        if (isOldAndLarge(h, now)) {
            ++o.oldAndLargeCount;
        }
    }
    return o;
}

std::vector<TreemapWeightItem> hierarchyToTreemapWeights(
    const std::vector<IndexHit>& children)
{
    std::vector<TreemapWeightItem> items;
    items.reserve(children.size());
    for (const auto& h : children) {
        if (h.size_bytes == 0) {
            continue;
        }
        TreemapWeightItem it;
        it.key = h.path;
        it.weight = static_cast<double>(h.size_bytes);
        it.tieBreak = h.path;
        items.push_back(std::move(it));
    }
    return items;
}

HierarchyChildrenResult queryHierarchyChildren(const std::wstring& rootPath,
                                               const std::wstring& locationPath,
                                               std::size_t childLimit)
{
    const auto t0 = std::chrono::steady_clock::now();
    HierarchyChildrenResult out;
    const std::wstring loc = normalizeLocation(rootPath, locationPath);
    out.locationPath = loc;

    auto summary = summarizeIndexedRoot(rootPath);
    if (!summary.exists) {
        out.error = "index_not_found";
        return out;
    }

    const bool atRoot = (loc == rootPath);
    if (atRoot) {
        out.locationLogicalBytes = summary.logicalBytes;
    } else {
        if (auto entry = lookupIndexEntry(rootPath, loc)) {
            out.locationLogicalBytes = entry->size_bytes;
        }
    }

    IndexQuerySpec spec;
    spec.includeFiles = true;
    spec.includeDirectories = true;
    spec.browsePath = loc;
    spec.sortBy = IndexSortKey::Size;
    spec.sortDescending = true;
    spec.limit = childLimit == 0 ? 10000 : childLimit;

    auto q = queryIndex(rootPath, spec);
    if (!q.ok) {
        out.error = q.error.empty() ? "query_failed" : q.error;
        return out;
    }
    out.children = std::move(q.hits);
    out.overview =
        buildStorageOverview(summary, loc, out.locationLogicalBytes, out.children);
    // Prefer root meta age from status when available.
    out.overview.snapshotAgeMs =
        q.age_ms != 0 ? q.age_ms : out.overview.snapshotAgeMs;
    if (out.locationLogicalBytes == 0 && atRoot) {
        out.locationLogicalBytes = summary.logicalBytes;
        out.overview.locationLogicalBytes = summary.logicalBytes;
    }
    out.ok = true;
    const auto t1 = std::chrono::steady_clock::now();
    out.query_elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}

}  // namespace spacelens
