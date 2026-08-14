#include "core/StorageAnalysis.hpp"

#include "core/DuplicateDetection.hpp"
#include "core/Json.hpp"
#include "core/ScanEngine.hpp"
#include "core/index/IndexPaths.hpp"
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
#define SPACELENS_VERSION_STRING "0.1.2"
#endif

namespace spacelens {
namespace {

constexpr int kSchemaVersion = 1;

IndexQueryResult queryKind(const std::wstring& root, bool files, bool dirs,
                           std::size_t limit, std::optional<ByteSize> minSize,
                           std::optional<std::uint64_t> olderThan,
                           const std::vector<std::string>& classifications)
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
    return queryIndex(root, spec);
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
        const auto status = indexStatus(request.root);
        if (!status.ok) {
            out.report.ok = false;
            out.report.source = EvidenceSource::PersistentIndex;
            out.report.state = "failed";
            out.report.error =
                status.error.empty() ? "index_not_found" : status.error;
            out.error = status.error == "index_not_found"
                            ? AnalysisError::IndexNotFound
                            : AnalysisError::ScanFailed;
            return out;
        }
        const auto dirs =
            queryKind(request.root, false, true, limit + 2, std::nullopt,
                      std::nullopt, {});
        const auto files =
            queryKind(request.root, true, false, limit + 1, std::nullopt,
                      std::nullopt, {});
        out.report = buildIndexedOverview(
            status.location.rootPath.empty() ? request.root
                                             : status.location.rootPath,
            status.root.logicalBytes, status.root.fileCount,
            status.root.dirCount, dirs.hits, files.hits, status.age_ms,
            status.root.indexedAtIso, limit);
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
        const auto status = indexStatus(request.root);
        if (!status.ok) {
            out.report.ok = false;
            out.report.source = EvidenceSource::PersistentIndex;
            out.report.state = "failed";
            out.report.error =
                status.error.empty() ? "index_not_found" : status.error;
            out.error = status.error == "index_not_found"
                            ? AnalysisError::IndexNotFound
                            : AnalysisError::ScanFailed;
            return out;
        }

        std::vector<IndexHit> hits;
        bool fetchCapped = false;
        auto appendUnique = [&](const IndexQueryResult& more) {
            if (more.hits.size() >= kIndexedOpportunityFetchLimit ||
                more.matched_items > more.returned_items) {
                fetchCapped = true;
            }
            for (const auto& hit : more.hits) {
                const bool exists = std::any_of(
                    hits.begin(), hits.end(),
                    [&](const IndexHit& have) { return have.path == hit.path; });
                if (!exists) {
                    hits.push_back(hit);
                }
            }
        };
        appendUnique(queryKind(request.root, true, true,
                               kIndexedOpportunityFetchLimit, query.minSize,
                               std::nullopt,
                               regenerableOpportunityClassifications()));
        appendUnique(queryKind(request.root, true, true,
                               kIndexedOpportunityFetchLimit, query.minSize,
                               query.olderThanDays, {}));
        IndexQuerySpec reclaimSpec;
        reclaimSpec.includeFiles = true;
        reclaimSpec.includeDirectories = true;
        reclaimSpec.minSize = query.minSize;
        reclaimSpec.limit = kIndexedOpportunityFetchLimit;
        reclaimSpec.candidateStrengths = {"Strong", "Moderate"};
        reclaimSpec.sortBy = IndexSortKey::CandidateStrength;
        reclaimSpec.sortDescending = true;
        appendUnique(queryIndex(request.root, reclaimSpec));

        out.report = buildIndexedOpportunities(
            status.location.rootPath.empty() ? request.root
                                             : status.location.rootPath,
            status.root.logicalBytes, status.root.fileCount,
            status.root.dirCount, hits, query, status.age_ms,
            status.root.indexedAtIso);
        if (fetchCapped) {
            out.report.uniqueReviewEstimated = true;
        }
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
       << "\"indexed_at\":" << jsonString(result.root.indexedAtIso) << ","
       << "\"age_ms\":" << jsonUInt(result.age_ms) << ","
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
       << "\"indexed_at\":" << jsonString(status.root.indexedAtIso) << ","
       << "\"full_indexed_at\":" << jsonString(status.root.indexedAtIso) << ","
       << "\"age_ms\":" << jsonUInt(status.age_ms) << ","
       << "\"snapshot_age_ms\":" << jsonUInt(status.age_ms) << ","
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
          "\"overview\",\"opportunities\",\"duplicates\","
          "\"capabilities\",\"help\",\"version\"],"
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
