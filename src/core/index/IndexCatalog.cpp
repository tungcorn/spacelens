#include "core/index/IndexCatalog.hpp"

#include "core/index/IndexStore.hpp"

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
    return spec;
}

}  // namespace spacelens
