#include "TestRunner.hpp"

#include "cli/Args.hpp"
#include "core/FileTime.hpp"
#include "core/StorageAnalysis.hpp"
#include "core/StorageIntelligence.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/index/IndexSnapshot.hpp"
#include "core/index/IndexStore.hpp"
#include "core/index/Sqlite.hpp"
#include "mcp/Protocol.hpp"
#include "mcp/StorageTools.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace spacelens;

namespace {

constexpr FileTimeTicks kNow = 133800000000000000ULL;

class IsolatedDataRoot {
public:
    IsolatedDataRoot()
    {
        wchar_t previous[32768]{};
        const DWORD n =
            ::GetEnvironmentVariableW(L"SPACELENS_DATA_ROOT", previous, 32768);
        if (n > 0 && n < 32768) {
            m_hadPrevious = true;
            m_previous.assign(previous, n);
        }
        namespace fs = std::filesystem;
        m_dir = fs::temp_directory_path() / "spacelens_freshness_appdata" /
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(m_dir);
        ::SetEnvironmentVariableW(L"SPACELENS_DATA_ROOT", m_dir.wstring().c_str());
    }

    ~IsolatedDataRoot()
    {
        if (m_hadPrevious) {
            ::SetEnvironmentVariableW(L"SPACELENS_DATA_ROOT", m_previous.c_str());
        } else {
            ::SetEnvironmentVariableW(L"SPACELENS_DATA_ROOT", nullptr);
        }
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    IsolatedDataRoot(const IsolatedDataRoot&) = delete;
    IsolatedDataRoot& operator=(const IsolatedDataRoot&) = delete;

private:
    std::filesystem::path m_dir;
    std::wstring m_previous;
    bool m_hadPrevious = false;
};

std::wstring makeRootDir(const char* tag)
{
    namespace fs = std::filesystem;
    const auto dir =
        fs::temp_directory_path() / "spacelens_freshness_root" / tag /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir.wstring();
}

void removeRootDir(const std::wstring& root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void publishSynthetic(const std::wstring& root, FileTimeTicks indexedAt = kNow)
{
    auto loc = locateIndex(root);
    {
        auto store = IndexStore::createStaging(loc);
        IndexRootInfo meta;
        meta.rootId = 1;
        meta.rootPath = loc.rootPath;
        meta.rootKey = loc.rootKey;
        meta.schemaVersion = kIndexSchemaVersion;
        meta.indexedAtTicks = indexedAt;
        meta.indexedAtIso = fileTimeTicksToIsoUtc(indexedAt);
        meta.fileCount = 1;
        meta.dirCount = 2;
        meta.logicalBytes = 8ULL * 1024ULL * 1024ULL;
        meta.status = IndexStatus::Ready;
        store.writeRootMeta(meta);

        SqliteStmt insert(
            store.db(),
            "INSERT INTO entries("
            "id, root_id, parent_id, kind, name, path, size_bytes, recursive_size, "
            "extension, last_write_ticks, last_access_ticks, attributes, is_reparse, "
            "classification, confidence, rule_id, location_safety, reclaimability, "
            "candidate_strength, newest_descendant_write, oldest_descendant_write, "
            "file_id, parent_file_id) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,"
            "?19,?20,?21,?22,?23);");

        SqliteTxn txn(store.db());
        auto add = [&](std::int64_t id, bool directory, const std::wstring& name,
                       const std::wstring& path, ByteSize size,
                       const char* classification, const char* strength) {
            insert.reset();
            insert.clearBindings();
            insert.bindInt64(1, id);
            insert.bindInt64(2, 1);
            insert.bindNull(3);
            insert.bindInt64(4, directory ? 1 : 0);
            insert.bindText16(5, name);
            insert.bindText16(6, path);
            insert.bindInt64(7, static_cast<std::int64_t>(size));
            insert.bindInt64(8, static_cast<std::int64_t>(size));
            insert.bindText(9, directory ? "" : "bin");
            insert.bindInt64(10, static_cast<std::int64_t>(indexedAt));
            insert.bindInt64(11, 0);
            insert.bindInt64(12, 0);
            insert.bindInt64(13, 0);
            insert.bindText(14, classification);
            insert.bindText(15, "High");
            insert.bindText(16, "nm");
            insert.bindText(17, "Ordinary");
            insert.bindText(18, "TypicallyRegenerable");
            insert.bindText(19, strength);
            insert.bindInt64(20, static_cast<std::int64_t>(indexedAt));
            insert.bindInt64(21, 0);
            insert.bindInt64(22, 0);
            insert.bindInt64(23, 0);
            insert.stepDone();
        };

        add(1, true, L"root", loc.rootPath, 8ULL * 1024ULL * 1024ULL, "Unknown",
            "None");
        const std::wstring nm = loc.rootPath + L"\\node_modules";
        add(2, true, L"node_modules", nm, 8ULL * 1024ULL * 1024ULL,
            "NodeModules", "Strong");
        add(3, false, L"left-pad.js", nm + L"\\left-pad.js",
            8ULL * 1024ULL * 1024ULL, "NodeModules", "Strong");
        txn.commit();
    }
    SPACELENS_REQUIRE(publishIndexDatabase(loc));
}

void writeCheckpoint(const std::wstring& root, FileTimeTicks lastRefresh,
                     const char* method, FileTimeTicks fullTicks)
{
    auto store = IndexStore::openReadWrite(locateIndex(root));
    SqliteStmt stmt(
        store.db(),
        "INSERT INTO refresh_checkpoint("
        "root_id, volume_device_path, volume_root_path, volume_serial, filesystem, "
        "usn_journal_id, next_usn, lowest_valid_usn, full_indexed_at_ticks, "
        "last_refresh_at_ticks, last_refresh_method, status) "
        "VALUES(1,'','',0,'',0,0,0,?1,?2,?3,'ready') "
        "ON CONFLICT(root_id) DO UPDATE SET "
        "full_indexed_at_ticks=excluded.full_indexed_at_ticks, "
        "last_refresh_at_ticks=excluded.last_refresh_at_ticks, "
        "last_refresh_method=excluded.last_refresh_method;");
    stmt.bindInt64(1, static_cast<std::int64_t>(fullTicks));
    stmt.bindInt64(2, static_cast<std::int64_t>(lastRefresh));
    stmt.bindText(3, method);
    stmt.stepDone();
}

spacelens::cli::ParsedArgs parseCli(std::vector<std::wstring> tokens)
{
    std::vector<wchar_t*> argv;
    argv.reserve(tokens.size());
    for (auto& t : tokens) {
        argv.push_back(t.data());
    }
    return spacelens::cli::parseArgs(static_cast<int>(argv.size()), argv.data());
}

bool jsonHasFreshTrue(const std::string& json)
{
    return json.find("\"fresh\":true") != std::string::npos ||
           json.find("\"fresh\": true") != std::string::npos;
}

}  // namespace

SPACELENS_TEST(Freshness_eval_known_unknown_skew)
{
    IndexPublishMetadata meta;
    meta.rootIndexedAtTicks = kNow;
    meta.rootIndexedAtIso = fileTimeTicksToIsoUtc(kNow);

    const auto known =
        evaluateIndexSnapshot(meta, kNow + 3600 * kFileTimeTicksPerSecond);
    SPACELENS_REQUIRE(known.ageState == SnapshotAgeState::Known);
    SPACELENS_REQUIRE(known.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*known.ageSeconds, 3600ULL);
    SPACELENS_REQUIRE(known.ageMs == 3600ULL * 1000ULL);
    SPACELENS_REQUIRE(known.publishKind == SnapshotPublishKind::Full);
    SPACELENS_REQUIRE(known.basis == "published_snapshot");

    const auto unknownNow = evaluateIndexSnapshot(meta, 0);
    SPACELENS_REQUIRE(unknownNow.ageState == SnapshotAgeState::Unknown);
    SPACELENS_REQUIRE(!unknownNow.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(unknownNow.ageMs, 0ULL);

    IndexPublishMetadata empty;
    const auto unknownPub = evaluateIndexSnapshot(empty, kNow);
    SPACELENS_REQUIRE(unknownPub.ageState == SnapshotAgeState::Unknown);

    const auto skew =
        evaluateIndexSnapshot(meta, kNow - kFileTimeTicksPerSecond);
    SPACELENS_REQUIRE(skew.ageState == SnapshotAgeState::ClockSkew);
    SPACELENS_REQUIRE(!skew.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(skew.ageMs, 0ULL);
}

SPACELENS_TEST(Freshness_eval_incremental_last_refresh_wins)
{
    IndexPublishMetadata meta;
    meta.rootIndexedAtTicks = kNow;
    meta.rootIndexedAtIso = fileTimeTicksToIsoUtc(kNow);
    meta.fullIndexedAtTicks = kNow;
    meta.lastRefreshAtTicks = kNow + 1800 * kFileTimeTicksPerSecond;
    meta.lastRefreshMethod = "usn";

    const auto ev =
        evaluateIndexSnapshot(meta, kNow + 3600 * kFileTimeTicksPerSecond);
    SPACELENS_REQUIRE(ev.publishKind == SnapshotPublishKind::Incremental);
    SPACELENS_REQUIRE(ev.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*ev.ageSeconds, 1800ULL);
    SPACELENS_REQUIRE(ev.publishedAtTicks == meta.lastRefreshAtTicks);
}

SPACELENS_TEST(Freshness_gate_eq_gt_unknown)
{
    IndexSnapshotEvidence known;
    known.ageState = SnapshotAgeState::Known;
    known.ageSeconds = 3600;
    known.ageMs = 3600 * 1000;

    const auto none = evaluateIndexAgeGate(known, std::nullopt);
    SPACELENS_REQUIRE(none.result == IndexAgeGateResult::NotRequested);

    const auto eq = evaluateIndexAgeGate(known, 3600);
    SPACELENS_REQUIRE(eq.result == IndexAgeGateResult::Satisfied);

    const auto tooOld = evaluateIndexAgeGate(known, 3599);
    SPACELENS_REQUIRE(tooOld.result == IndexAgeGateResult::TooOld);

    IndexSnapshotEvidence unknown;
    unknown.ageState = SnapshotAgeState::Unknown;
    const auto unk = evaluateIndexAgeGate(unknown, 10);
    SPACELENS_REQUIRE(unk.result == IndexAgeGateResult::FreshnessUnknown);

    IndexSnapshotEvidence skew;
    skew.ageState = SnapshotAgeState::ClockSkew;
    const auto skewGate = evaluateIndexAgeGate(skew, 10);
    SPACELENS_REQUIRE(skewGate.result == IndexAgeGateResult::FreshnessUnknown);

    const std::string err = formatIndexAgeGateError(tooOld);
    SPACELENS_REQUIRE(err.find("3600") != std::string::npos);
    SPACELENS_REQUIRE(err.find("No refresh was performed") != std::string::npos);
}

SPACELENS_TEST(Freshness_json_has_no_bare_fresh)
{
    IndexSnapshotEvidence ev;
    ev.ageState = SnapshotAgeState::Known;
    ev.publishKind = SnapshotPublishKind::Full;
    ev.publishedAtUtc = "2026-08-15T00:00:00Z";
    ev.ageSeconds = 10;
    ev.ageMs = 10000;
    IndexAgeDecision decision;
    decision.result = IndexAgeGateResult::Satisfied;
    decision.evidence = ev;
    decision.requestedMaxAgeSeconds = 60;
    const auto json = indexFreshnessJsonObject(ev, decision);
    SPACELENS_REQUIRE(json.find("\"basis\":\"published_snapshot\"") !=
                      std::string::npos);
    SPACELENS_REQUIRE(json.find("\"age_state\":\"known\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"max_age_satisfied\":true") !=
                      std::string::npos);
    SPACELENS_REQUIRE(!jsonHasFreshTrue(json));
    SPACELENS_REQUIRE(json.find("\"fresh\"") == std::string::npos);
}

SPACELENS_TEST(Freshness_full_build_and_incremental_published_age)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("full");
    publishSynthetic(root, kNow);

    const FileTimeTicks later = kNow + 3600 * kFileTimeTicksPerSecond;
    const auto status = indexStatus(root, later);
    SPACELENS_REQUIRE(status.ok);
    SPACELENS_REQUIRE(status.snapshot.ageState == SnapshotAgeState::Known);
    SPACELENS_REQUIRE(status.snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*status.snapshot.ageSeconds, 3600ULL);
    SPACELENS_REQUIRE(status.ageDecision.result ==
                      IndexAgeGateResult::NotRequested);

    writeCheckpoint(root, kNow + 1800 * kFileTimeTicksPerSecond, "usn", kNow);
    const auto after = indexStatus(root, later);
    SPACELENS_REQUIRE(after.ok);
    SPACELENS_REQUIRE(after.snapshot.publishKind ==
                      SnapshotPublishKind::Incremental);
    SPACELENS_REQUIRE(after.snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*after.snapshot.ageSeconds, 1800ULL);

    removeRootDir(root);
}

SPACELENS_TEST(Freshness_failed_refresh_does_not_advance)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("failrefresh");
    publishSynthetic(root, kNow);
    const FileTimeTicks refreshAt = kNow + 600 * kFileTimeTicksPerSecond;
    writeCheckpoint(root, refreshAt, "usn", kNow);

    const auto before =
        indexStatus(root, refreshAt + 100 * kFileTimeTicksPerSecond);
    SPACELENS_REQUIRE(before.snapshot.publishedAtTicks == refreshAt);

    // A failed refresh writes nothing. Published ticks must stay put.
    const auto after =
        indexStatus(root, refreshAt + 500 * kFileTimeTicksPerSecond);
    SPACELENS_REQUIRE(after.snapshot.publishedAtTicks == refreshAt);
    SPACELENS_REQUIRE(after.snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*after.snapshot.ageSeconds, 500ULL);

    removeRootDir(root);
}

SPACELENS_TEST(Freshness_query_gate_before_hits)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("querygate");
    publishSynthetic(root, kNow);
    const FileTimeTicks later = kNow + 3600 * kFileTimeTicksPerSecond;

    IndexQuerySpec okSpec;
    okSpec.includeFiles = true;
    okSpec.nowTicks = later;
    okSpec.maxIndexAgeSeconds = 3600;
    okSpec.limit = 10;
    const auto ok = queryIndex(root, okSpec);
    SPACELENS_REQUIRE(ok.ok);
    SPACELENS_REQUIRE(!ok.hits.empty());
    SPACELENS_REQUIRE(ok.ageDecision.result == IndexAgeGateResult::Satisfied);

    IndexQuerySpec stale;
    stale.includeFiles = true;
    stale.nowTicks = later;
    stale.maxIndexAgeSeconds = 3599;
    stale.limit = 10;
    const auto rejected = queryIndex(root, stale);
    SPACELENS_REQUIRE(!rejected.ok);
    SPACELENS_REQUIRE(rejected.error == "index_too_old");
    SPACELENS_REQUIRE(rejected.hits.empty());
    SPACELENS_REQUIRE_EQ(rejected.matched_items, 0ULL);
    SPACELENS_REQUIRE(rejected.ageDecision.result == IndexAgeGateResult::TooOld);

    const auto json = indexQueryToJson(rejected);
    SPACELENS_REQUIRE(json.find("\"error\":\"index_too_old\"") !=
                      std::string::npos);
    SPACELENS_REQUIRE(json.find("\"freshness\"") != std::string::npos);
    SPACELENS_REQUIRE(!jsonHasFreshTrue(json));
    SPACELENS_REQUIRE(json.find("\"schema_version\":1") != std::string::npos);

    IndexQuerySpec noPolicy;
    noPolicy.includeFiles = true;
    noPolicy.nowTicks = later;
    noPolicy.limit = 10;
    const auto open = queryIndex(root, noPolicy);
    SPACELENS_REQUIRE(open.ok);
    SPACELENS_REQUIRE(open.ageDecision.result ==
                      IndexAgeGateResult::NotRequested);

    removeRootDir(root);
}

SPACELENS_TEST(Freshness_opportunities_gate_before_stream)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("oppgate");
    publishSynthetic(root, kNow);
    const FileTimeTicks later = kNow + 7200 * kFileTimeTicksPerSecond;

    IndexedOpportunitySpec stale;
    stale.nowTicks = later;
    stale.maxIndexAgeSeconds = 60;
    stale.limit = 5;
    const auto fetched = queryIndexedOpportunities(root, stale);
    SPACELENS_REQUIRE(!fetched.ok);
    SPACELENS_REQUIRE(fetched.error == "index_too_old");
    SPACELENS_REQUIRE(fetched.topHits.empty());
    SPACELENS_REQUIRE_EQ(fetched.rowsStreamed, 0ULL);
    SPACELENS_REQUIRE_EQ(fetched.uniqueReviewBytes, 0ULL);

    OpportunityRequest req;
    req.root = root;
    req.fromIndex = true;
    req.query.nowTicks = later;
    req.maxIndexAgeSeconds = 60;
    const auto analysis = analyzeOpportunities(req);
    SPACELENS_REQUIRE(analysis.error == AnalysisError::IndexTooOld);
    SPACELENS_REQUIRE(!analysis.report.ok);
    SPACELENS_REQUIRE(analysis.report.error == "index_too_old");
    SPACELENS_REQUIRE(analysis.report.opportunities.empty());
    const auto json = analysis.report.toJson();
    SPACELENS_REQUIRE(json.find("\"freshness\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"error\":\"index_too_old\"") !=
                      std::string::npos);
    SPACELENS_REQUIRE(!jsonHasFreshTrue(json));

    removeRootDir(root);
}

SPACELENS_TEST(Freshness_overview_unknown_and_skew_fail_closed)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("ovgate");
    publishSynthetic(root, kNow);

    OverviewRequest skew;
    skew.root = root;
    skew.fromIndex = true;
    skew.nowTicks = kNow - kFileTimeTicksPerSecond;
    skew.maxIndexAgeSeconds = 60;
    const auto skewOut = analyzeOverview(skew);
    SPACELENS_REQUIRE(skewOut.error == AnalysisError::IndexFreshnessUnknown);
    SPACELENS_REQUIRE(skewOut.report.error == "index_freshness_unknown");
    SPACELENS_REQUIRE(skewOut.report.largestDirectories.empty());
    SPACELENS_REQUIRE(skewOut.report.largestFiles.empty());

    IndexQuerySpec unknownNow;
    unknownNow.includeFiles = true;
    unknownNow.nowTicks = 0;
    unknownNow.maxIndexAgeSeconds = 60;
    // queryIndex substitutes nowFileTime() when nowTicks==0, so force
    // unknown via a published timestamp of 0 instead.
    removeRootDir(root);
}

SPACELENS_TEST(Freshness_zero_published_ticks_unknown_fail_closed)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("zeropub");
    publishSynthetic(root, 0);
    IndexQuerySpec spec;
    spec.includeFiles = true;
    spec.nowTicks = kNow;
    spec.maxIndexAgeSeconds = 60;
    const auto rejected = queryIndex(root, spec);
    SPACELENS_REQUIRE(!rejected.ok);
    SPACELENS_REQUIRE(rejected.error == "index_freshness_unknown");
    SPACELENS_REQUIRE(rejected.hits.empty());
    SPACELENS_REQUIRE(rejected.snapshot.ageState == SnapshotAgeState::Unknown);

    removeRootDir(root);
}

SPACELENS_TEST(Freshness_duplicates_exempt_from_max_age)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("dup");
    publishSynthetic(root, kNow);
    const auto candidates =
        queryDuplicateSizeCandidates(root, 1024ULL * 1024ULL);
    SPACELENS_REQUIRE(candidates.ok || candidates.error.empty() ||
                      candidates.error != "index_too_old");
    SPACELENS_REQUIRE(candidates.error != "index_too_old");
    SPACELENS_REQUIRE(candidates.error != "index_freshness_unknown");

    removeRootDir(root);
}

SPACELENS_TEST(Freshness_status_never_applies_gate)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("status");
    publishSynthetic(root, kNow);
    const auto status =
        indexStatus(root, kNow + 86400 * kFileTimeTicksPerSecond);
    SPACELENS_REQUIRE(status.ok);
    SPACELENS_REQUIRE(status.ageDecision.result ==
                      IndexAgeGateResult::NotRequested);
    const auto json = indexStatusToJson(status, {});
    SPACELENS_REQUIRE(json.find("\"freshness\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"full_indexed_at\"") != std::string::npos);
    SPACELENS_REQUIRE(!jsonHasFreshTrue(json));

    removeRootDir(root);
}

SPACELENS_TEST(Freshness_cli_max_age_parse)
{
    using spacelens::cli::Command;
    auto q = parseCli({L"spacelens", L"query", L"D:\\data", L"--files",
                       L"--max-index-age-seconds", L"3600"});
    SPACELENS_REQUIRE(q.error.empty());
    SPACELENS_REQUIRE(q.command == Command::Query);
    SPACELENS_REQUIRE(q.maxIndexAgeSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*q.maxIndexAgeSeconds, 3600ULL);

    auto o = parseCli({L"spacelens", L"overview", L"D:\\data", L"--from-index",
                       L"--max-index-age-seconds", L"0", L"--json"});
    SPACELENS_REQUIRE(o.error.empty());
    SPACELENS_REQUIRE(o.maxIndexAgeSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*o.maxIndexAgeSeconds, 0ULL);

    auto p = parseCli({L"spacelens", L"opportunities", L"D:\\data",
                       L"--from-index", L"--max-index-age-seconds", L"10"});
    SPACELENS_REQUIRE(p.error.empty());

    auto live = parseCli({L"spacelens", L"overview", L"D:\\data",
                          L"--max-index-age-seconds", L"10"});
    SPACELENS_REQUIRE(!live.error.empty());

    auto scan = parseCli({L"spacelens", L"scan", L"D:\\data",
                          L"--max-index-age-seconds", L"10"});
    SPACELENS_REQUIRE(!scan.error.empty());

    auto dups = parseCli({L"spacelens", L"duplicates", L"D:\\data",
                          L"--max-index-age-seconds", L"10"});
    SPACELENS_REQUIRE(!dups.error.empty());

    auto status = parseCli({L"spacelens", L"index", L"status", L"D:\\data",
                            L"--max-index-age-seconds", L"10"});
    SPACELENS_REQUIRE(!status.error.empty());

    auto missing = parseCli({L"spacelens", L"query", L"D:\\data", L"--files",
                             L"--max-index-age-seconds"});
    SPACELENS_REQUIRE(!missing.error.empty());

    auto fresh = parseCli({L"spacelens", L"query", L"D:\\data", L"--files",
                           L"--fresh"});
    SPACELENS_REQUIRE(!fresh.error.empty());

    const auto help = spacelens::cli::helpText();
    SPACELENS_REQUIRE(help.find("--max-index-age-seconds") != std::string::npos);
    SPACELENS_REQUIRE(help.find("--fresh") == std::string::npos);
    SPACELENS_REQUIRE(help.find("--stale-after") == std::string::npos);
}

SPACELENS_TEST(Freshness_mcp_schema_and_live_scan_rejected)
{
    using namespace spacelens::mcp;
    McpServer server;
    registerStorageTools(server);
    SPACELENS_REQUIRE_EQ(server.tools().size(), 6ULL);
    for (const auto& tool : server.tools()) {
        SPACELENS_REQUIRE(tool.name != "storage_freshness");
        SPACELENS_REQUIRE(tool.name != "storage_refresh");
        SPACELENS_REQUIRE(tool.name != "index_refresh");
        SPACELENS_REQUIRE(tool.name != "storage_index_refresh");
        SPACELENS_REQUIRE(tool.name != "storage_require_fresh");
        SPACELENS_REQUIRE(tool.name != "storage_wait_for_index");
        const std::string schema = tool.inputSchema.stringify();
        const bool expects =
            tool.name == "storage_overview" ||
            tool.name == "storage_opportunities" ||
            tool.name == "storage_query";
        const bool has = schema.find("max_index_age_seconds") != std::string::npos;
        SPACELENS_REQUIRE(has == expects);
    }

    const auto line = server.handleLine(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"storage_overview\",\"arguments\":{"
        "\"path\":\"C:\\\\Windows\",\"source\":\"live_scan\","
        "\"max_index_age_seconds\":60}}}");
    SPACELENS_REQUIRE(line.has_value());
    SPACELENS_REQUIRE(line->find("max_index_age_seconds") != std::string::npos);
    SPACELENS_REQUIRE(line->find("persistent_index") != std::string::npos);
}
