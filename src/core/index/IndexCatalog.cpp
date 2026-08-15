#include "core/index/IndexCatalog.hpp"

#include "core/Json.hpp"
#include "core/index/IndexStore.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <sstream>

namespace spacelens {

const char* toString(IndexFreshness f) noexcept
{
    switch (f) {
    case IndexFreshness::Missing:
        return "missing";
    case IndexFreshness::Fresh:
        return "fresh";
    case IndexFreshness::AgedSnapshot:
        return "aged_snapshot";
    case IndexFreshness::RefreshAvailable:
        return "refresh_available";
    case IndexFreshness::IncrementalUnavailable:
        return "incremental_unavailable";
    case IndexFreshness::FullRebuildRequired:
        return "full_rebuild_required";
    case IndexFreshness::Error:
        return "error";
    }
    return "error";
}

const char* toString(IndexDiscoveryPreset p) noexcept
{
    switch (p) {
    case IndexDiscoveryPreset::Custom:
        return "custom";
    case IndexDiscoveryPreset::Largest:
        return "largest";
    case IndexDiscoveryPreset::OldAndLarge:
        return "old_and_large";
    case IndexDiscoveryPreset::DeveloperStorage:
        return "developer_storage";
    case IndexDiscoveryPreset::ReclaimCandidates:
        return "reclaim_candidates";
    }
    return "custom";
}

IndexFreshness mapIndexFreshness(bool indexExists, std::uint64_t ageMs,
                                 IncrementalRefreshState incState,
                                 IndexRefreshOutcome probeOutcome)
{
    if (!indexExists || probeOutcome == IndexRefreshOutcome::IndexNotFound) {
        return IndexFreshness::Missing;
    }
    if (probeOutcome == IndexRefreshOutcome::Failed) {
        return IndexFreshness::Error;
    }

    // Live USN incremental is available.
    if (incState == IncrementalRefreshState::Supported) {
        return IndexFreshness::RefreshAvailable;
    }

    // USN cannot run (privilege / journal / FS) — snapshot remains queryable.
    // Prefer this over outcome==FullRebuildRequired, which refresh also uses
    // as a generic "cannot incrementally refresh" signal.
    if (incState == IncrementalRefreshState::AccessDenied ||
        incState == IncrementalRefreshState::JournalNotActive ||
        incState == IncrementalRefreshState::UnsupportedFilesystem ||
        incState == IncrementalRefreshState::Unavailable) {
        return IndexFreshness::IncrementalUnavailable;
    }

    // True discontinuity / explicit rebuild need.
    if (incState == IncrementalRefreshState::JournalChanged ||
        incState == IncrementalRefreshState::HistoryLost ||
        incState == IncrementalRefreshState::VolumeChanged ||
        incState == IncrementalRefreshState::NeedsFullRebuild ||
        probeOutcome == IndexRefreshOutcome::FullRebuildRequired) {
        return IndexFreshness::FullRebuildRequired;
    }

    // Index exists; no strong USN signal → age-based label only.
    if (ageMs <= kFreshAgeMs) {
        return IndexFreshness::Fresh;
    }
    return IndexFreshness::AgedSnapshot;
}

namespace {

const char* freshnessLabel(IndexFreshness f)
{
    switch (f) {
    case IndexFreshness::Missing:
        return "Missing";
    case IndexFreshness::Fresh:
        return "Fresh snapshot";
    case IndexFreshness::AgedSnapshot:
        return "Aged snapshot";
    case IndexFreshness::RefreshAvailable:
        return "Refresh available";
    case IndexFreshness::IncrementalUnavailable:
        return "Incremental unavailable";
    case IndexFreshness::FullRebuildRequired:
        return "Full rebuild required";
    case IndexFreshness::Error:
        return "Error";
    }
    return "Error";
}

IndexRootSummary fillFromStatusAndProbe(const std::wstring& rootPath)
{
    IndexRootSummary s;
    s.rootPath = rootPath;
    auto loc = locateIndex(rootPath);
    s.rootKey = loc.rootKey;
    s.dbPath = loc.dbPath;
    s.exists = indexDatabaseExists(loc);

    auto st = indexStatus(rootPath);
    if (st.ok) {
        s.ageMs = st.age_ms;
        s.fileCount = st.root.fileCount;
        s.dirCount = st.root.dirCount;
        s.logicalBytes = st.root.logicalBytes;
        s.indexedAtIso = st.root.indexedAtIso;
        s.fullIndexedAtIso = st.root.indexedAtIso;
    }

    auto probe = probeIncremental(rootPath);
    s.incrementalState = probe.incrementalState;
    s.incrementalSupported =
        probe.incrementalState == IncrementalRefreshState::Supported;
    s.reason = probe.reason;
    s.checkpointStatus = probe.checkpoint.status;
    s.lastRefreshMethod = probe.checkpoint.lastRefreshMethod;
    if (!probe.root.indexedAtIso.empty()) {
        s.indexedAtIso = probe.root.indexedAtIso;
    }

    s.freshness = mapIndexFreshness(s.exists, s.ageMs, probe.incrementalState,
                                    probe.outcome);
    s.freshnessLabel = freshnessLabel(s.freshness);
    return s;
}

}  // namespace

IndexRootSummary summarizeIndexedRoot(const std::wstring& rootPath)
{
    return fillFromStatusAndProbe(rootPath);
}

const char* toString(IndexCatalogStatus status) noexcept
{
    switch (status) {
    case IndexCatalogStatus::Ready:
        return "ready";
    case IndexCatalogStatus::Incompatible:
        return "incompatible";
    case IndexCatalogStatus::Unavailable:
        return "unavailable";
    case IndexCatalogStatus::Corrupt:
        return "corrupt";
    }
    return "unavailable";
}

namespace {

FileTimeTicks catalogNowTicks()
{
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

IndexPublishMetadata publishMetadataFrom(const IndexRootInfo& root, SqliteDb& db)
{
    IndexPublishMetadata meta;
    meta.rootIndexedAtTicks = root.indexedAtTicks;
    meta.rootIndexedAtIso = root.indexedAtIso;
    if (auto cp = readRefreshCheckpoint(db)) {
        meta.lastRefreshAtTicks = cp->lastRefreshAtTicks;
        meta.lastRefreshMethod = cp->lastRefreshMethod;
        meta.fullIndexedAtTicks = cp->fullIndexedAtTicks;
    }
    return meta;
}

IndexCatalogEntry inspectListedIndex(const ListedIndex& listed, FileTimeTicks now)
{
    IndexCatalogEntry entry;
    entry.rootKey = listed.rootKey;
    entry.dbPath = listed.dbPath;
    entry.root = listed.rootPath;

    IndexLocation loc;
    loc.rootKey = listed.rootKey;
    loc.dbPath = listed.dbPath;
    const auto slash = listed.dbPath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        loc.indexDir = listed.dbPath.substr(0, slash);
    }

    try {
        auto store = IndexStore::openInspect(loc);
        entry.indexSchemaVersion = store.schemaVersion();
        auto meta = store.readRootMeta();
        if (!meta) {
            entry.status = IndexCatalogStatus::Corrupt;
            entry.reason = "index_corrupt";
            return entry;
        }
        entry.root = meta->rootPath;
        entry.fileCount = meta->fileCount;
        entry.directoryCount = meta->dirCount;
        entry.logicalBytes = meta->logicalBytes;
        entry.snapshot = evaluateIndexSnapshot(publishMetadataFrom(*meta, store.db()),
                                               now);
        entry.hasPublishedSnapshot = true;
        if (entry.indexSchemaVersion != kIndexSchemaVersion) {
            entry.status = IndexCatalogStatus::Incompatible;
            entry.reason = "unsupported_schema";
            return entry;
        }
        if (meta->status != IndexStatus::Ready) {
            entry.status = IndexCatalogStatus::Unavailable;
            entry.reason = "index_not_ready";
            return entry;
        }
        entry.status = IndexCatalogStatus::Ready;
        return entry;
    } catch (...) {
        entry.status = IndexCatalogStatus::Unavailable;
        entry.reason = "index_open_failed";
        return entry;
    }
}

bool catalogEntryLess(const IndexCatalogEntry& a, const IndexCatalogEntry& b)
{
    if (a.root.empty() != b.root.empty()) {
        return !a.root.empty();
    }
    if (a.root.empty()) {
        return compareIndexRootPath(a.rootKey, b.rootKey) < 0;
    }
    const int cmp = compareIndexRootPath(a.root, b.root);
    if (cmp != 0) {
        return cmp < 0;
    }
    return compareIndexRootPath(a.rootKey, b.rootKey) < 0;
}

}  // namespace

IndexCatalogListing listPublishedIndexes(FileTimeTicks nowTicks)
{
    IndexCatalogListing listing;
    listing.nowTicks = nowTicks != 0 ? nowTicks : catalogNowTicks();
    const auto listed = listIndexedRoots();
    listing.indexes.reserve(listed.size());
    for (const auto& item : listed) {
        listing.indexes.push_back(inspectListedIndex(item, listing.nowTicks));
    }
    std::sort(listing.indexes.begin(), listing.indexes.end(), catalogEntryLess);
    return listing;
}

std::string indexCatalogToJson(const IndexCatalogListing& listing)
{
    std::ostringstream os;
    os << "{"
       << "\"schema_version\":1,"
       << "\"ok\":true,"
       << "\"command\":\"index_list\","
       << "\"source\":\"persistent_index\","
       << "\"indexes\":[";
    bool first = true;
    for (const auto& entry : listing.indexes) {
        if (!first) {
            os << ',';
        }
        first = false;
        os << "{"
           << "\"root\":" << jsonString(entry.root) << ','
           << "\"path\":" << jsonString(entry.dbPath) << ','
           << "\"root_key\":" << jsonString(entry.rootKey) << ','
           << "\"index_schema_version\":" << entry.indexSchemaVersion << ','
           << "\"status\":" << jsonString(toString(entry.status));
        if (!entry.reason.empty()) {
            os << ",\"reason\":" << jsonString(entry.reason)
               << ",\"error\":" << jsonString(entry.reason);
        }
        if (entry.fileCount) {
            os << ",\"file_count\":" << jsonUInt(*entry.fileCount);
        }
        if (entry.directoryCount) {
            os << ",\"directory_count\":" << jsonUInt(*entry.directoryCount);
        }
        if (entry.logicalBytes) {
            os << ",\"logical_bytes\":" << jsonUInt(*entry.logicalBytes);
        }
        if (entry.hasPublishedSnapshot) {
            if (!entry.snapshot.publishedAtUtc.empty()) {
                os << ",\"indexed_at\":" << jsonString(entry.snapshot.publishedAtUtc);
            }
            IndexAgeDecision decision;
            decision.evidence = entry.snapshot;
            os << ",\"freshness\":"
               << indexFreshnessJsonObject(entry.snapshot, decision);
        }
        os << '}';
    }
    os << "]}\n";
    return os.str();
}

std::string formatIndexCatalogHuman(const IndexCatalogListing& listing)
{
    if (listing.indexes.empty()) {
        return "No published indexes under " +
               utf8FromWide(spaceLensIndexesRoot()) + "\n";
    }
    std::ostringstream os;
    for (const auto& entry : listing.indexes) {
        if (entry.root.empty()) {
            os << "(unreadable " << utf8FromWide(entry.rootKey) << ')';
        } else {
            os << utf8FromWide(entry.root);
        }
        os << "  " << toString(entry.status);
        if (entry.hasPublishedSnapshot) {
            if (entry.snapshot.ageState == SnapshotAgeState::Known &&
                entry.snapshot.ageSeconds) {
                os << "  " << *entry.snapshot.ageSeconds << 's';
            } else if (entry.snapshot.ageState == SnapshotAgeState::ClockSkew) {
                os << "  clock skew";
            } else {
                os << "  age unknown";
            }
            if (entry.snapshot.publishKind != SnapshotPublishKind::Unknown) {
                os << "  " << toString(entry.snapshot.publishKind);
            }
        }
        os << '\n';
    }
    return os.str();
}

std::vector<IndexRootSummary> listIndexSummaries()
{
    std::vector<IndexRootSummary> out;
    for (auto& listed : listIndexedRoots()) {
        std::wstring rootPath = listed.rootPath;
        if (rootPath.empty()) {
            try {
                IndexLocation loc;
                loc.rootKey = listed.rootKey;
                loc.dbPath = listed.dbPath;
                const auto slash = listed.dbPath.find_last_of(L"\\/");
                if (slash != std::wstring::npos) {
                    loc.indexDir = listed.dbPath.substr(0, slash + 1);
                }
                auto store = IndexStore::openRead(loc);
                if (auto meta = store.readRootMeta()) {
                    rootPath = meta->rootPath;
                }
            } catch (...) {
                IndexRootSummary bad;
                bad.rootKey = listed.rootKey;
                bad.dbPath = listed.dbPath;
                bad.exists = true;
                bad.freshness = IndexFreshness::Error;
                bad.freshnessLabel = "Error";
                bad.reason = "index_open_failed";
                out.push_back(std::move(bad));
                continue;
            }
        }
        if (rootPath.empty()) {
            continue;
        }
        out.push_back(summarizeIndexedRoot(rootPath));
    }
    return out;
}

IndexQuerySpec makeBrowserQuerySpec(bool includeFiles, bool includeDirectories,
                                    std::optional<ByteSize> minSize,
                                    std::string extensionLowerNoDot,
                                    std::string classification,
                                    std::string candidateStrength,
                                    std::size_t limit)
{
    IndexQuerySpec spec;
    spec.includeFiles = includeFiles;
    spec.includeDirectories = includeDirectories;
    spec.minSize = minSize;
    spec.extension = std::move(extensionLowerNoDot);
    spec.classification = std::move(classification);
    spec.candidateStrength = std::move(candidateStrength);
    spec.limit = limit == 0 ? 20 : limit;
    spec.sortBy = IndexSortKey::Size;
    spec.sortDescending = true;
    return spec;
}

const std::vector<std::string>& developerStorageClassifications()
{
    static const std::vector<std::string> kCats = {
        "BuildArtifact",     "DependencyDirectory", "PackageCache",
        "IdeCache",          "DownloadedAiModel",   "LogData",
    };
    return kCats;
}

IndexQuerySpec applyDiscoveryPreset(IndexDiscoveryPreset preset,
                                    IndexQuerySpec base)
{
    IndexQuerySpec spec = std::move(base);
    if (spec.limit == 0) {
        spec.limit = 200;
    }

    switch (preset) {
    case IndexDiscoveryPreset::Largest:
        // Bare applyDiscoveryPreset(Largest, {}) defaults to both kinds.
        spec.includeFiles = true;
        spec.includeDirectories = true;
        spec.sortBy = IndexSortKey::Size;
        spec.sortDescending = true;
        break;

    case IndexDiscoveryPreset::OldAndLarge:
        if (!spec.minSize) {
            spec.minSize = kOldAndLargeMinBytes;
        }
        if (!spec.olderThanDays) {
            spec.olderThanDays = kOldAndLargeOlderThanDays;
        }
        spec.includeFiles = true;
        spec.includeDirectories = true;
        spec.sortBy = IndexSortKey::Size;
        spec.sortDescending = true;
        break;

    case IndexDiscoveryPreset::DeveloperStorage:
        if (spec.classifications.empty() && spec.classification.empty()) {
            spec.classifications = developerStorageClassifications();
        }
        spec.includeFiles = true;
        spec.includeDirectories = true;
        spec.sortBy = IndexSortKey::Size;
        spec.sortDescending = true;
        break;

    case IndexDiscoveryPreset::ReclaimCandidates:
        if (spec.candidateStrengths.empty() && spec.candidateStrength.empty()) {
            spec.candidateStrengths = {"Strong", "Moderate"};
        }
        spec.includeFiles = true;
        spec.includeDirectories = true;
        spec.sortBy = IndexSortKey::CandidateStrength;
        spec.sortDescending = true;
        break;

    case IndexDiscoveryPreset::Custom:
    default:
        if (!spec.includeFiles && !spec.includeDirectories) {
            spec.includeFiles = true;
            spec.includeDirectories = false;
        }
        break;
    }
    return spec;
}

IndexQuerySpec makeDiscoveryQuery(
    IndexDiscoveryPreset preset, bool includeFiles, bool includeDirectories,
    std::optional<ByteSize> minSize, std::optional<std::uint64_t> olderThanDays,
    std::string extensionLowerNoDot, std::string classification,
    std::string candidateStrength, std::string searchText,
    std::wstring browsePath, IndexSortKey sortBy, bool sortDescending,
    std::size_t limit)
{
    IndexQuerySpec base;
    base.minSize = minSize;
    base.olderThanDays = olderThanDays;
    base.extension = std::move(extensionLowerNoDot);
    base.classification = std::move(classification);
    base.candidateStrength = std::move(candidateStrength);
    base.searchText = std::move(searchText);
    base.browsePath = std::move(browsePath);
    base.limit = limit == 0 ? 200 : limit;
    base.sortBy = sortBy;
    base.sortDescending = sortDescending;

    auto spec = applyDiscoveryPreset(preset, std::move(base));

    // GUI kind selection always wins over preset defaults.
    spec.includeFiles = includeFiles;
    spec.includeDirectories = includeDirectories;
    if (!spec.includeFiles && !spec.includeDirectories) {
        spec.includeFiles = true;
    }

    // GUI sort combo always wins (presets set the combo when selected).
    spec.sortBy = sortBy;
    spec.sortDescending = sortDescending;
    return spec;
}

}  // namespace spacelens
