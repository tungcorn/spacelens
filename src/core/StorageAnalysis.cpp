#include "core/StorageAnalysis.hpp"

#include "core/DuplicateDetection.hpp"
#include "core/Json.hpp"
#include "core/ScanEngine.hpp"
#include "core/index/IndexBreakdown.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexSnapshot.hpp"
#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/FileContentHasher.hpp"
#include "platform/windows/WindowsFileEnumerator.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <sstream>

#ifndef SPACELENS_VERSION_STRING
#define SPACELENS_VERSION_STRING "0.1.8"
#endif

namespace spacelens {
namespace {

constexpr int kSchemaVersion = 1;

IndexQueryResult queryKind(const std::wstring& root, bool files, bool dirs,
                           std::size_t limit, std::optional<ByteSize> minSize,
                           std::optional<std::uint64_t> olderThan,
                           const std::vector<std::string>& classifications,
                           FileTimeTicks nowTicks = 0,
                           std::optional<std::uint64_t> maxIndexAgeSeconds = {})
{
    IndexQuerySpec spec;
    spec.includeFiles = files;
    spec.includeDirectories = dirs;
    spec.limit = limit;
    spec.minSize = minSize;
    spec.olderThanDays = olderThan;
    spec.classifications = classifications;
    spec.sortBy = IndexSortKey::Size;
    spec.sortDescending = true;
    spec.nowTicks = nowTicks;
    spec.maxIndexAgeSeconds = maxIndexAgeSeconds;
    return queryIndex(root, spec);
}

AnalysisError analysisErrorFromIndex(const std::string& error)
{
    if (error == "index_not_found") {
        return AnalysisError::IndexNotFound;
    }
    if (error == "cancelled") {
        return AnalysisError::Cancelled;
    }
    if (error == "index_too_old") {
        return AnalysisError::IndexTooOld;
    }
    if (error == "index_freshness_unknown") {
        return AnalysisError::IndexFreshnessUnknown;
    }
    return AnalysisError::ScanFailed;
}

void applySnapshotToOverview(StorageOverviewReport& report,
                             const IndexQueryResult& status)
{
    report.snapshot = status.snapshot;
    report.ageDecision = status.ageDecision;
    report.indexAgeMs = status.snapshot.ageMs;
    report.indexedAtIso = status.snapshot.publishedAtUtc;
}

void applySnapshotToOpportunities(OpportunityReport& report,
                                  const IndexedOpportunityFetch& fetched)
{
    report.snapshot = fetched.snapshot;
    report.ageDecision = fetched.ageDecision;
    report.indexAgeMs = fetched.snapshot.ageMs;
    report.indexedAtIso = fetched.snapshot.publishedAtUtc;
}

ScanResult scanRoot(const std::wstring& path, std::size_t topFiles,
                    std::stop_token stop)
{
    WindowsFileEnumerator enumerator;
    ScanEngine engine(enumerator);
    ScanOptions options;
    options.topFileCount = topFiles;
    return engine.scan(path, options, stop);
}

std::wstring userProfilePath()
{
    wchar_t profile[MAX_PATH]{};
    const DWORD profileLen =
        ::GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (profileLen > 0 && profileLen < MAX_PATH) {
        return std::wstring(profile, profileLen);
    }
    return {};
}

}  // namespace

const char* toString(AnalysisError error) noexcept
{
    switch (error) {
    case AnalysisError::None:
        return "ok";
    case AnalysisError::InaccessibleRoot:
        return "inaccessible_root";
    case AnalysisError::IndexNotFound:
        return "index_not_found";
    case AnalysisError::ScanFailed:
        return "scan_failed";
    case AnalysisError::Cancelled:
        return "cancelled";
    case AnalysisError::InvalidArgument:
        return "invalid_argument";
    case AnalysisError::IndexTooOld:
        return "index_too_old";
    case AnalysisError::IndexFreshnessUnknown:
        return "index_freshness_unknown";
    }
    return "unknown";
}

bool pathExists(const std::wstring& path)
{
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool pathIsDirectory(const std::wstring& path)
{
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

FileTimeTicks currentFileTime()
{
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

bool parseEvidenceSource(std::string_view text, bool& fromIndex,
                         std::string& error)
{
    if (text.empty() || text == "live_scan") {
        fromIndex = false;
        return true;
    }
    if (text == "persistent_index") {
        fromIndex = true;
        return true;
    }
    error = "source must be live_scan or persistent_index";
    return false;
}

OverviewAnalysis analyzeOverview(const OverviewRequest& request,
                                 std::stop_token stop)
{
    OverviewAnalysis out;
    out.report.root = request.root;
    const std::size_t limit = request.limit == 0 ? kDefaultOverviewLimit
                                                 : request.limit;
    if (!pathExists(request.root) || !pathIsDirectory(request.root)) {
        out.error = AnalysisError::InaccessibleRoot;
        out.report.ok = false;
        out.report.state = "failed";
        out.report.error = "inaccessible_root";
        out.report.source = request.fromIndex ? EvidenceSource::PersistentIndex
                                              : EvidenceSource::LiveScan;
        return out;
    }

    if (request.fromIndex) {
        const FileTimeTicks now =
            request.nowTicks != 0 ? request.nowTicks : currentFileTime();
        const auto status = indexStatus(request.root, now);
        if (!status.ok) {
            out.report.ok = false;
            out.report.source = EvidenceSource::PersistentIndex;
            out.report.state = "failed";
            out.report.error =
                status.error.empty() ? "index_not_found" : status.error;
            out.error = analysisErrorFromIndex(out.report.error);
            applySnapshotToOverview(out.report, status);
            return out;
        }
        applySnapshotToOverview(out.report, status);
        const auto gate =
            evaluateIndexAgeGate(status.snapshot, request.maxIndexAgeSeconds);
        out.report.ageDecision = gate;
        if (gate.result == IndexAgeGateResult::TooOld ||
            gate.result == IndexAgeGateResult::FreshnessUnknown) {
            out.report.ok = false;
            out.report.source = EvidenceSource::PersistentIndex;
            out.report.state = "failed";
            out.report.error = gate.result == IndexAgeGateResult::TooOld
                                   ? "index_too_old"
                                   : "index_freshness_unknown";
            out.error = analysisErrorFromIndex(out.report.error);
            return out;
        }
        const auto dirs = queryKind(
            request.root, false, true, limit + 2, std::nullopt, std::nullopt, {},
            now, request.maxIndexAgeSeconds);
        if (!dirs.ok) {
            out.report.ok = false;
            out.report.source = EvidenceSource::PersistentIndex;
            out.report.state = "failed";
            out.report.error = dirs.error.empty() ? "index_query_failed" : dirs.error;
            out.error = analysisErrorFromIndex(out.report.error);
            applySnapshotToOverview(out.report, dirs);
            return out;
        }
        const auto files = queryKind(
            request.root, true, false, limit + 1, std::nullopt, std::nullopt, {},
            now, request.maxIndexAgeSeconds);
        if (!files.ok) {
            out.report.ok = false;
            out.report.source = EvidenceSource::PersistentIndex;
            out.report.state = "failed";
            out.report.error =
                files.error.empty() ? "index_query_failed" : files.error;
            out.error = analysisErrorFromIndex(out.report.error);
            applySnapshotToOverview(out.report, files);
            return out;
        }
        out.report = buildIndexedOverview(
            status.location.rootPath.empty() ? request.root
                                             : status.location.rootPath,
            status.root.logicalBytes, status.root.fileCount,
            status.root.dirCount, dirs.hits, files.hits, status.snapshot.ageMs,
            status.snapshot.publishedAtUtc, limit);
        applySnapshotToOverview(out.report, status);
        out.report.ageDecision = gate;
        return out;
    }

    const std::size_t topFiles = std::max<std::size_t>(limit, 100);
    auto result = scanRoot(request.root, topFiles, stop);
    out.report = buildLiveOverview(result, limit, currentFileTime());
    out.report.accessDenied = result.progress.accessDenied;
    out.report.reparseSkipped = result.progress.reparsePointsSkipped;
    out.report.otherErrors = result.progress.otherErrors;
    if (result.state == ScanState::Cancelled) {
        out.error = AnalysisError::Cancelled;
    } else if (result.state == ScanState::Failed) {
        out.error = AnalysisError::ScanFailed;
    }
    return out;
}

OpportunityAnalysis analyzeOpportunities(const OpportunityRequest& request,
                                         std::stop_token stop)
{
    OpportunityAnalysis out;
    OpportunityQuery query = request.query;
    if (query.nowTicks == 0) {
        query.nowTicks = currentFileTime();
    }
    if (query.limit == 0) {
        query.limit = kDefaultOpportunityLimit;
    }

    out.report.root = request.root;
    if (!pathExists(request.root) || !pathIsDirectory(request.root)) {
        out.error = AnalysisError::InaccessibleRoot;
        out.report.ok = false;
        out.report.state = "failed";
        out.report.error = "inaccessible_root";
        out.report.source = request.fromIndex ? EvidenceSource::PersistentIndex
                                              : EvidenceSource::LiveScan;
        return out;
    }

    if (request.fromIndex) {
        const auto status = indexStatus(request.root, query.nowTicks);
        if (!status.ok) {
            out.report.ok = false;
            out.report.source = EvidenceSource::PersistentIndex;
            out.report.state = "failed";
            out.report.error =
                status.error.empty() ? "index_not_found" : status.error;
            out.error = analysisErrorFromIndex(out.report.error);
            out.report.snapshot = status.snapshot;
            out.report.ageDecision = status.ageDecision;
            out.report.indexAgeMs = status.snapshot.ageMs;
            out.report.indexedAtIso = status.snapshot.publishedAtUtc;
            return out;
        }

        out.report.snapshot = status.snapshot;
        out.report.indexAgeMs = status.snapshot.ageMs;
        out.report.indexedAtIso = status.snapshot.publishedAtUtc;
        const auto gate =
            evaluateIndexAgeGate(status.snapshot, request.maxIndexAgeSeconds);
        out.report.ageDecision = gate;
        if (gate.result == IndexAgeGateResult::TooOld ||
            gate.result == IndexAgeGateResult::FreshnessUnknown) {
            out.report.ok = false;
            out.report.source = EvidenceSource::PersistentIndex;
            out.report.state = "failed";
            out.report.error = gate.result == IndexAgeGateResult::TooOld
                                   ? "index_too_old"
                                   : "index_freshness_unknown";
            out.error = analysisErrorFromIndex(out.report.error);
            return out;
        }

        if (stop.stop_requested()) {
            out.error = AnalysisError::Cancelled;
            out.report.ok = false;
            out.report.state = "cancelled";
            out.report.source = EvidenceSource::PersistentIndex;
            return out;
        }

        IndexedOpportunitySpec spec;
        spec.minSize = query.minSize;
        spec.olderThanDays = query.olderThanDays;
        spec.nowTicks = query.nowTicks;
        spec.limit = query.limit;
        spec.matchNone = query.matchNone;
        spec.pathPrefix = query.pathPrefix;
        spec.excludePath = status.location.rootPath.empty() ? request.root
                                                            : status.location.rootPath;
        spec.aggregateLimit = kIndexedOpportunityAggregateLimit;
        spec.maxIndexAgeSeconds = request.maxIndexAgeSeconds;
        if (query.categoryOnly) {
            spec.classification = toString(*query.categoryOnly);
        }

        const auto fetched = queryIndexedOpportunities(request.root, spec, stop);
        if (!fetched.ok) {
            out.report.ok = false;
            out.report.source = EvidenceSource::PersistentIndex;
            out.report.state = fetched.error == "cancelled" ? "cancelled" : "failed";
            out.report.error =
                fetched.error.empty() ? "index_query_failed" : fetched.error;
            out.error = analysisErrorFromIndex(out.report.error);
            applySnapshotToOpportunities(out.report, fetched);
            return out;
        }

        IndexedOpportunityExtras extras;
        extras.matchedCount = fetched.matchedItems;
        extras.hasStreamedAggregate = true;
        extras.uniqueReviewBytes = fetched.uniqueReviewBytes;
        extras.uniqueReviewEstimated = fetched.uniqueReviewEstimated;
        extras.aggregateOverflow = fetched.aggregateOverflow;
        extras.overlappedPathKeys = fetched.overlappedTopKeys;
        extras.rowsStreamed = fetched.rowsStreamed;
        extras.maxActiveDepth = fetched.maxActiveDepth;
        extras.groups.reserve(fetched.groups.size());
        for (const auto& group : fetched.groups) {
            OpportunityGroup copy;
            copy.id = group.id;
            copy.classification = group.classification;
            copy.logicalBytes = group.logicalBytes;
            copy.itemCount = group.itemCount;
            copy.estimated = group.estimated;
            copy.strongestCandidateStrength = group.strongestCandidateStrength;
            copy.reasonCodes = group.reasonCodes;
            extras.groups.push_back(std::move(copy));
        }

        out.report = buildIndexedOpportunities(
            status.location.rootPath.empty() ? request.root
                                             : status.location.rootPath,
            status.root.logicalBytes, status.root.fileCount,
            status.root.dirCount, fetched.topHits, query, fetched.snapshot.ageMs,
            fetched.snapshot.publishedAtUtc, extras);
        applySnapshotToOpportunities(out.report, fetched);
        out.report.elapsedMs = fetched.query_elapsed_ms;
        return out;
    }

    auto result = scanRoot(request.root, /*topFiles=*/0, stop);
    if (result.state == ScanState::Completed ||
        result.state == ScanState::Cancelled) {
        out.report = buildLiveOpportunities(result.tree, query);
        out.report.state =
            result.state == ScanState::Cancelled ? "cancelled" : "completed";
        out.report.ok = result.state == ScanState::Completed;
    } else {
        out.report.ok = false;
        out.report.state = "failed";
        out.report.root = request.root;
    }
    out.report.accessDenied = result.progress.accessDenied;
    out.report.reparseSkipped = result.progress.reparsePointsSkipped;
    out.report.otherErrors = result.progress.otherErrors;
    if (result.progress.elapsedSeconds > 0.0) {
        out.report.elapsedMs = static_cast<std::uint64_t>(
            result.progress.elapsedSeconds * 1000.0 + 0.5);
    }
    if (result.state == ScanState::Cancelled) {
        out.error = AnalysisError::Cancelled;
    } else if (result.state == ScanState::Failed) {
        out.error = AnalysisError::ScanFailed;
    }
    return out;
}

DuplicateDetectionResult analyzeDuplicates(const DuplicateRequest& request,
                                           std::stop_token stop)
{
    DuplicateScanOptions options;
    options.minimumSize = request.minSize;
    options.userProfilePath = userProfilePath();
    options.hashCachePath = spaceLensHashCachePath();
    const auto candidates =
        queryDuplicateSizeCandidates(request.root, options.minimumSize);
    if (!candidates.ok) {
        DuplicateDetectionResult failed;
        failed.root = candidates.root.rootPath.empty() ? request.root
                                                       : candidates.root.rootPath;
        failed.minimumSize = options.minimumSize;
        failed.error = candidates.error;
        failed.completed = false;
        return failed;
    }

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    return detectDuplicates(candidates, reader, hasher, options,
                            [&stop]() { return stop.stop_requested(); });
}

IndexStatusDocument analyzeIndexStatus(const std::wstring& root)
{
    IndexStatusDocument doc;
    doc.status = indexStatus(root);
    doc.probe = probeIncremental(root);
    return doc;
}

std::string indexQueryToJson(const IndexQueryResult& result)
{
    std::ostringstream os;
    os << "{"
       << "\"schema_version\":" << kSchemaVersion << ","
       << "\"ok\":" << jsonBool(result.ok) << ","
       << "\"command\":\"query\","
       << "\"source\":\"persistent_index\","
       << "\"root\":" << jsonString(result.location.rootPath) << ","
       << "\"index\":{"
       << "\"path\":" << jsonString(result.location.dbPath) << ","
       << "\"indexed_at\":"
       << jsonString(result.snapshot.publishedAtUtc.empty()
                         ? result.root.indexedAtIso
                         : result.snapshot.publishedAtUtc)
       << ","
       << "\"age_ms\":" << jsonUInt(result.age_ms) << ","
       << "\"freshness\":"
       << indexFreshnessJsonObject(result.snapshot, result.ageDecision) << ","
       << "\"index_schema_version\":" << kIndexSchemaVersion << "},"
       << "\"matched_items\":" << jsonUInt(result.matched_items) << ","
       << "\"returned_items\":" << jsonUInt(result.returned_items) << ","
       << "\"matched_logical_bytes\":" << jsonUInt(result.matched_logical_bytes)
       << ","
       << "\"results\":[";
    for (std::size_t i = 0; i < result.hits.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        const auto& h = result.hits[i];
        os << "{\"path\":" << jsonString(h.path) << ",\"kind\":"
           << jsonString(h.kind == IndexEntryKind::Directory ? "directory"
                                                            : "file")
           << ",\"size_bytes\":" << jsonUInt(h.size_bytes)
           << ",\"classification\":" << jsonString(h.classification)
           << ",\"confidence\":" << jsonString(h.confidence)
           << ",\"location_safety\":" << jsonString(h.location_safety)
           << ",\"reclaimability\":" << jsonString(h.reclaimability)
           << ",\"candidate_strength\":" << jsonString(h.candidate_strength)
           << "}";
    }
    os << "]";
    if (!result.error.empty()) {
        os << ",\"error\":" << jsonString(result.error);
    }
    os << "}\n";
    return os.str();
}

std::string indexBreakdownToJson(const IndexedBreakdown& result)
{
    std::ostringstream os;
    os << "{"
       << "\"schema_version\":" << kSchemaVersion << ","
       << "\"ok\":" << jsonBool(result.ok) << ","
       << "\"command\":\"breakdown\","
       << "\"source\":\"persistent_index\","
       << "\"root\":" << jsonString(result.location.rootPath) << ","
       << "\"under\":"
       << (result.under.empty() ? std::string("null") : jsonString(result.under))
       << ","
       << "\"limit\":" << jsonUInt(result.limit) << ","
       << "\"total_file_count\":" << jsonUInt(result.total_file_count) << ","
       << "\"total_logical_bytes\":" << jsonUInt(result.total_logical_bytes) << ","
       << "\"logical_bytes_estimated\":"
       << jsonBool(result.logical_bytes_estimated) << ","
       << "\"by_classification\":[";
    for (std::size_t i = 0; i < result.by_classification.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        const auto& g = result.by_classification[i];
        os << "{\"classification\":" << jsonString(g.key)
           << ",\"file_count\":" << jsonUInt(g.file_count)
           << ",\"logical_bytes\":" << jsonUInt(g.logical_bytes) << "}";
    }
    os << "],\"by_extension\":{\"groups\":[";
    for (std::size_t i = 0; i < result.by_extension.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        const auto& g = result.by_extension[i];
        os << "{\"extension\":" << jsonString(g.key)
           << ",\"file_count\":" << jsonUInt(g.file_count)
           << ",\"logical_bytes\":" << jsonUInt(g.logical_bytes) << "}";
    }
    os << "],\"other\":{"
       << "\"extension_groups\":"
       << jsonUInt(result.extension_other.extension_groups)
       << ",\"file_count\":" << jsonUInt(result.extension_other.file_count)
       << ",\"logical_bytes\":" << jsonUInt(result.extension_other.logical_bytes)
       << "}},\"by_last_write_age\":[";
    for (std::size_t i = 0; i < result.by_last_write_age.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        const auto& g = result.by_last_write_age[i];
        os << "{\"bucket\":" << jsonString(g.key)
           << ",\"file_count\":" << jsonUInt(g.file_count)
           << ",\"logical_bytes\":" << jsonUInt(g.logical_bytes) << "}";
    }
    os << "],\"index\":{"
       << "\"path\":" << jsonString(result.location.dbPath) << ","
       << "\"indexed_at\":"
       << jsonString(result.snapshot.publishedAtUtc.empty()
                         ? result.root.indexedAtIso
                         : result.snapshot.publishedAtUtc)
       << ","
       << "\"age_ms\":" << jsonUInt(result.snapshot.ageMs) << ","
       << "\"freshness\":"
       << indexFreshnessJsonObject(result.snapshot, result.ageDecision) << ","
       << "\"index_schema_version\":" << kIndexSchemaVersion << "}";
    if (!result.error.empty()) {
        os << ",\"error\":" << jsonString(result.error);
    }
    os << "}\n";
    return os.str();
}

std::string indexStatusToJson(const IndexQueryResult& status,
                              const IndexRefreshResult& probe)
{
    std::ostringstream os;
    os << "{"
       << "\"schema_version\":" << kSchemaVersion << ","
       << "\"ok\":" << jsonBool(status.ok) << ","
       << "\"command\":\"index_status\","
       << "\"root\":" << jsonString(status.location.rootPath) << ","
       << "\"source\":\"persistent_index\","
       << "\"index_schema_version\":" << kIndexSchemaVersion << ","
       << "\"index\":{"
       << "\"path\":" << jsonString(status.location.dbPath) << ","
       << "\"exists\":" << jsonBool(indexDatabaseExists(status.location)) << ","
       << "\"indexed_at\":"
       << jsonString(status.snapshot.publishedAtUtc.empty()
                         ? status.root.indexedAtIso
                         : status.snapshot.publishedAtUtc)
       << ","
       << "\"full_indexed_at\":"
       << jsonString(status.snapshot.fullIndexedAtUtc.empty()
                         ? status.root.indexedAtIso
                         : status.snapshot.fullIndexedAtUtc)
       << ","
       << "\"age_ms\":" << jsonUInt(status.age_ms) << ","
       << "\"snapshot_age_ms\":" << jsonUInt(status.age_ms) << ","
       << "\"freshness\":"
       << indexFreshnessJsonObject(status.snapshot, status.ageDecision) << ","
       << "\"file_count\":" << jsonUInt(status.root.fileCount) << ","
       << "\"directory_count\":" << jsonUInt(status.root.dirCount) << ","
       << "\"logical_bytes\":" << jsonUInt(status.root.logicalBytes) << ","
       << "\"status\":" << jsonString(toString(status.root.status)) << "},"
       << "\"incremental_refresh\":{"
       << "\"supported\":"
       << jsonBool(probe.incrementalState == IncrementalRefreshState::Supported)
       << ","
       << "\"state\":" << jsonString(toString(probe.incrementalState)) << ","
       << "\"reason\":" << jsonString(probe.reason) << ","
       << "\"fallback\":"
       << jsonString(probe.incrementalState == IncrementalRefreshState::Supported
                         ? "none"
                         : "full_rebuild")
       << ","
       << "\"refresh_method\":"
       << jsonString(probe.checkpoint.lastRefreshMethod) << ","
       << "\"last_incremental_refresh\":"
       << jsonString(fileTimeTicksToIsoUtc(probe.checkpoint.lastRefreshAtTicks))
       << ","
       << "\"checkpoint\":{"
       << "\"usn_journal_id\":" << jsonUInt(probe.checkpoint.usnJournalId) << ","
       << "\"next_usn\":" << jsonUInt(probe.checkpoint.nextUsn) << ","
       << "\"volume_serial\":" << jsonUInt(probe.checkpoint.volumeSerial) << ","
       << "\"status\":" << jsonString(probe.checkpoint.status) << "}"
       << "}";
    if (!status.error.empty()) {
        os << ",\"error\":" << jsonString(status.error);
    }
    os << "}\n";
    return os.str();
}

std::string cliCapabilitiesJson()
{
    std::ostringstream os;
    os << "{"
       << "\"schema_version\":" << kSchemaVersion << ","
       << "\"version\":" << jsonString(SPACELENS_VERSION_STRING) << ","
       << "\"json_contract_version\":" << kSchemaVersion << ","
       << "\"commands\":[\"scan\",\"top\",\"find\",\"index\","
          "\"index status\",\"index list\",\"index refresh\",\"query\","
          "\"overview\",\"opportunities\",\"breakdown\",\"reclaim-plan\","
          "\"duplicates\",\"capabilities\",\"help\",\"version\"],"
       << "\"features\":{"
       << "\"json\":true,"
       << "\"cancellation\":true,"
       << "\"persistent_index\":true,"
       << "\"indexed_query\":true,"
       << "\"incremental_index\":true,"
       << "\"filesystem_mutation\":false,"
       << "\"classification\":true,"
       << "\"filters\":true,"
       << "\"storage_overview\":true,"
       << "\"storage_opportunities\":true,"
       << "\"indexed_breakdown\":true,"
       << "\"reclaim_plan\":true,"
       << "\"duplicate_detection\":true,"
       << "\"reclaim_analysis\":true"
       << "},"
       << "\"read_only\":true,"
       << "\"filesystem_mutation\":false,"
       << "\"index_schema_version\":" << kIndexSchemaVersion << "}\n";
    return os.str();
}

std::string mcpCapabilitiesJson()
{
    std::ostringstream os;
    os << "{"
       << "\"schema_version\":" << kSchemaVersion << ","
       << "\"version\":" << jsonString(SPACELENS_VERSION_STRING) << ","
       << "\"json_contract_version\":" << kSchemaVersion << ","
       << "\"interface\":\"mcp\","
       << "\"transport\":\"stdio\","
       << "\"protocol_versions\":[\"2026-07-28\",\"2025-11-25\"],"
       << "\"tools\":[\"storage_capabilities\",\"storage_overview\","
          "\"storage_opportunities\",\"storage_query\",\"storage_duplicates\","
          "\"storage_index_status\"],"
       << "\"features\":{"
       << "\"json\":true,"
       << "\"cancellation\":true,"
       << "\"persistent_index\":true,"
       << "\"indexed_query\":true,"
       << "\"incremental_index\":true,"
       << "\"filesystem_mutation\":false,"
       << "\"classification\":true,"
       << "\"filters\":true,"
       << "\"storage_overview\":true,"
       << "\"storage_opportunities\":true,"
       << "\"duplicate_detection\":true,"
       << "\"reclaim_analysis\":true,"
       << "\"mcp\":true,"
       << "\"read_only_mcp\":true,"
       << "\"index_refresh\":false,"
       << "\"embedded_model\":false"
       << "},"
       << "\"read_only\":true,"
       << "\"filesystem_mutation\":false,"
       << "\"planning_only\":true,"
       << "\"index_schema_version\":" << kIndexSchemaVersion << "}\n";
    return os.str();
}

}  // namespace spacelens
