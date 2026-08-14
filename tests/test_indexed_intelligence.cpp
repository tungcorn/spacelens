#include "TestRunner.hpp"

#include "core/FileTime.hpp"
#include "core/StorageAnalysis.hpp"
#include "core/StorageIntelligence.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/index/IndexStore.hpp"
#include "core/index/Sqlite.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "core/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <limits>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace spacelens;

namespace {

constexpr FileTimeTicks kNow = 133800000000000000ULL;

struct SynthRow {
    std::wstring relPath;
    bool directory = false;
    ByteSize size = 0;
    std::string classification = "Unknown";
    std::string confidence = "Low";
    std::string ruleId;
    std::string safety = "Ordinary";
    std::string reclaim = "Unknown";
    std::string strength = "ReviewOnly";
    FileTimeTicks writeTicks = 0;
};

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
        m_dir = fs::temp_directory_path() / "spacelens_idx_intel_appdata" /
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
        fs::temp_directory_path() / "spacelens_idx_intel_root" / tag /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir.wstring();
}

void removeRootDir(const std::wstring& root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

std::wstring joinRoot(const std::wstring& root, const std::wstring& rel)
{
    if (rel.empty()) {
        return root;
    }
    std::wstring out = root;
    if (out.back() != L'\\' && out.back() != L'/') {
        out.push_back(L'\\');
    }
    out += rel;
    return out;
}

std::wstring leafName(const std::wstring& rel)
{
    const auto pos = rel.find_last_of(L"\\/");
    return pos == std::wstring::npos ? rel : rel.substr(pos + 1);
}

void publishSynthetic(const std::wstring& root, const std::vector<SynthRow>& rows)
{
    auto loc = locateIndex(root);
    ByteSize logical = 0;
    std::uint64_t files = 0;
    std::uint64_t dirs = 1;
    for (const auto& row : rows) {
        if (row.directory) {
            ++dirs;
        } else {
            ++files;
        }
        logical += row.size;
    }

    {
        auto store = IndexStore::createStaging(loc);
        IndexRootInfo meta;
        meta.rootId = 1;
        meta.rootPath = loc.rootPath;
        meta.rootKey = loc.rootKey;
        meta.schemaVersion = kIndexSchemaVersion;
        meta.indexedAtTicks = kNow;
        meta.indexedAtIso = fileTimeTicksToIsoUtc(kNow);
        meta.fileCount = files;
        meta.dirCount = dirs;
        meta.logicalBytes = logical;
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
        std::int64_t id = 1;
        insert.reset();
        insert.clearBindings();
        insert.bindInt64(1, id++);
        insert.bindInt64(2, 1);
        insert.bindNull(3);
        insert.bindInt64(4, 1);
        insert.bindText16(5, leafName(root));
        insert.bindText16(6, loc.rootPath);
        insert.bindInt64(7, static_cast<std::int64_t>(logical));
        insert.bindInt64(8, static_cast<std::int64_t>(logical));
        insert.bindText(9, "");
        insert.bindInt64(10, static_cast<std::int64_t>(kNow));
        insert.bindInt64(11, 0);
        insert.bindInt64(12, 0);
        insert.bindInt64(13, 0);
        insert.bindText(14, "Unknown");
        insert.bindText(15, "Low");
        insert.bindText(16, "");
        insert.bindText(17, "Ordinary");
        insert.bindText(18, "Unknown");
        insert.bindText(19, "None");
        insert.bindInt64(20, static_cast<std::int64_t>(kNow));
        insert.bindInt64(21, 0);
        insert.bindInt64(22, 0);
        insert.bindInt64(23, 0);
        insert.stepDone();

        for (const auto& row : rows) {
            const std::wstring path = joinRoot(loc.rootPath, row.relPath);
            insert.reset();
            insert.clearBindings();
            insert.bindInt64(1, id++);
            insert.bindInt64(2, 1);
            insert.bindNull(3);
            insert.bindInt64(4, row.directory ? 1 : 0);
            insert.bindText16(5, leafName(row.relPath));
            insert.bindText16(6, path);
            insert.bindInt64(7, static_cast<std::int64_t>(row.size));
            insert.bindInt64(8, static_cast<std::int64_t>(row.size));
            insert.bindText(9, "");
            insert.bindInt64(10, static_cast<std::int64_t>(row.writeTicks));
            insert.bindInt64(11, 0);
            insert.bindInt64(12, 0);
            insert.bindInt64(13, 0);
            insert.bindText(14, row.classification);
            insert.bindText(15, row.confidence);
            insert.bindText(16, row.ruleId);
            insert.bindText(17, row.safety);
            insert.bindText(18, row.reclaim);
            insert.bindText(19, row.strength);
            insert.bindInt64(20, static_cast<std::int64_t>(row.writeTicks));
            insert.bindInt64(21, 0);
            insert.bindInt64(22, 0);
            insert.bindInt64(23, 0);
            insert.stepDone();
        }
        txn.commit();
    }
    SPACELENS_REQUIRE(publishIndexDatabase(loc));
}

std::wstring indexedRoot(const std::wstring& root)
{
    return locateIndex(root).rootPath;
}

IndexHit hitFromRow(const std::wstring& root, const SynthRow& row)
{
    IndexHit hit;
    hit.path = joinRoot(indexedRoot(root), row.relPath);
    hit.name = leafName(row.relPath);
    hit.kind = row.directory ? IndexEntryKind::Directory : IndexEntryKind::File;
    hit.size_bytes = row.size;
    hit.classification = row.classification;
    hit.confidence = row.confidence;
    hit.rule_id = row.ruleId;
    hit.location_safety = row.safety;
    hit.reclaimability = row.reclaim;
    hit.candidate_strength = row.strength;
    hit.last_write_ticks = row.writeTicks;
    return hit;
}

OpportunityReport oracleFromRows(const std::wstring& root, ByteSize logical,
                                 const std::vector<SynthRow>& rows,
                                 const OpportunityQuery& query)
{
    std::vector<IndexHit> hits;
    hits.reserve(rows.size());
    for (const auto& row : rows) {
        hits.push_back(hitFromRow(root, row));
    }
    return buildIndexedOpportunities(root, logical, 0, 0, hits, query, 0, {});
}

SynthRow oldLargeFile(std::wstring rel, ByteSize size)
{
    SynthRow row;
    row.relPath = std::move(rel);
    row.directory = false;
    row.size = size;
    row.classification = "Unknown";
    row.confidence = "Low";
    row.safety = "Ordinary";
    row.reclaim = "Unknown";
    row.strength = "ReviewOnly";
    row.writeTicks = kNow - daysToTicks(200);
    return row;
}

SynthRow hiddenDep(std::wstring rel, ByteSize size)
{
    SynthRow row;
    row.relPath = std::move(rel);
    row.directory = true;
    row.size = size;
    row.classification = "DependencyDirectory";
    row.confidence = "High";
    row.ruleId = "node-modules";
    row.safety = "Ordinary";
    row.reclaim = "LikelyRegenerable";
    row.strength = "Strong";
    row.writeTicks = kNow - daysToTicks(10);
    return row;
}

bool reportHasPath(const OpportunityReport& report, const std::wstring& needle)
{
    for (const auto& item : report.opportunities) {
        if (item.path.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

OpportunityAnalysis queryIndexed(const std::wstring& root, std::size_t limit,
                                 std::optional<StorageCategory> cls = {},
                                 std::wstring under = {},
                                 ByteSize minSize = 1,
                                 std::uint64_t olderThan = 90)
{
    OpportunityRequest request;
    request.root = root;
    request.fromIndex = true;
    request.query.minSize = minSize;
    request.query.olderThanDays = olderThan;
    request.query.nowTicks = kNow;
    request.query.limit = limit;
    request.query.categoryOnly = cls;
    request.query.pathPrefix = std::move(under);
    return analyzeOpportunities(request);
}

}  // namespace

SPACELENS_TEST(IndexedIntel_hidden_candidate_after_old_prefetch_cap)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("after_cap");
    std::vector<SynthRow> rows;
    rows.reserve(kIndexedOpportunityFetchLimit + 2);
    for (std::size_t i = 0; i < kIndexedOpportunityFetchLimit + 1; ++i) {
        rows.push_back(oldLargeFile(L"decoy-" + std::to_wstring(i) + L".bin",
                                    50ULL * 1024ULL * 1024ULL + i));
    }
    rows.push_back(hiddenDep(L"app\\node_modules", 12ULL * 1024ULL * 1024ULL));
    publishSynthetic(root, rows);

    const auto analysis = queryIndexed(root, 5);
    SPACELENS_REQUIRE(analysis.error == AnalysisError::None);
    SPACELENS_REQUIRE(analysis.report.ok);
    SPACELENS_REQUIRE(reportHasPath(analysis.report, L"node_modules"));
    SPACELENS_REQUIRE(analysis.report.opportunities.front().classification ==
                      "DependencyDirectory");
    SPACELENS_REQUIRE(analysis.report.rankingPolicy == kOpportunityRankPolicy);
    removeRootDir(root);
}

SPACELENS_TEST(IndexedIntel_hidden_candidate_at_final_row)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("final_row");
    constexpr int kCount = 10000;
    std::vector<SynthRow> rows;
    rows.reserve(kCount + 1);
    for (int i = 0; i < kCount; ++i) {
        rows.push_back(oldLargeFile(L"old\\file-" + std::to_wstring(i) + L".bin",
                                    2ULL * 1024ULL * 1024ULL + static_cast<ByteSize>(i)));
    }
    rows.push_back(hiddenDep(L"late\\node_modules", 8ULL * 1024ULL * 1024ULL));
    publishSynthetic(root, rows);

    const auto top5 = queryIndexed(root, 5);
    SPACELENS_REQUIRE(top5.report.ok);
    SPACELENS_REQUIRE(reportHasPath(top5.report, L"late\\node_modules"));
    SPACELENS_REQUIRE(top5.report.opportunities.front().candidateStrength == "Strong");
    SPACELENS_REQUIRE(top5.report.truncated);
    removeRootDir(root);
}

SPACELENS_TEST(IndexedIntel_classification_filter_ignores_unrelated_prefix)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("class_filter");
    std::vector<SynthRow> rows;
    for (int i = 0; i < 10000; ++i) {
        SynthRow build;
        build.relPath = L"cmake-build-" + std::to_wstring(i);
        build.directory = true;
        build.size = 4ULL * 1024ULL * 1024ULL;
        build.classification = "BuildArtifact";
        build.confidence = "Medium";
        build.ruleId = "cmake-build-prefix";
        build.reclaim = "LikelyRegenerable";
        build.strength = "ReviewOnly";
        build.writeTicks = kNow - daysToTicks(5);
        rows.push_back(std::move(build));
    }
    SynthRow cache;
    cache.relPath = L".cache";
    cache.directory = true;
    cache.size = 3ULL * 1024ULL * 1024ULL;
    cache.classification = "TemporaryData";
    cache.confidence = "Medium";
    cache.ruleId = "dot-cache";
    cache.reclaim = "PossiblyRegenerable";
    cache.strength = "ReviewOnly";
    cache.writeTicks = kNow - daysToTicks(5);
    rows.push_back(cache);
    publishSynthetic(root, rows);

    const auto filtered =
        queryIndexed(root, 20, StorageCategory::TemporaryData);
    SPACELENS_REQUIRE(filtered.report.ok);
    SPACELENS_REQUIRE_EQ(filtered.report.opportunities.size(), 1ULL);
    SPACELENS_REQUIRE(filtered.report.opportunities.front().classification ==
                      "TemporaryData");
    SPACELENS_REQUIRE(reportHasPath(filtered.report, L".cache"));

    OpportunityRequest none;
    none.root = root;
    none.fromIndex = true;
    none.query.minSize = 1;
    none.query.nowTicks = kNow;
    none.query.matchNone = true;
    const auto empty = analyzeOpportunities(none);
    SPACELENS_REQUIRE(empty.report.ok);
    SPACELENS_REQUIRE(empty.report.opportunities.empty());
    removeRootDir(root);
}

SPACELENS_TEST(IndexedIntel_unfiltered_topn_matches_oracle)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("oracle10k");
    std::vector<SynthRow> rows;
    ByteSize logical = 0;
    for (int i = 0; i < 10000; ++i) {
        auto decoy = oldLargeFile(L"mix\\old-" + std::to_wstring(i) + L".bin",
                                  3ULL * 1024ULL * 1024ULL + static_cast<ByteSize>(i * 100));
        logical += decoy.size;
        rows.push_back(std::move(decoy));
    }
    auto rust = hiddenDep(L"rust-app\\target", 90ULL * 1024ULL * 1024ULL);
    rust.classification = "BuildArtifact";
    rust.ruleId = "rust-target-dir";
    rust.strength = "Moderate";
    logical += rust.size;
    rows.push_back(rust);
    auto node = hiddenDep(L"app\\node_modules", 80ULL * 1024ULL * 1024ULL);
    logical += node.size;
    rows.push_back(node);
    auto cache = hiddenDep(L"tool\\.cache", 20ULL * 1024ULL * 1024ULL);
    cache.classification = "TemporaryData";
    cache.confidence = "Medium";
    cache.strength = "ReviewOnly";
    cache.reclaim = "PossiblyRegenerable";
    logical += cache.size;
    rows.push_back(cache);
    publishSynthetic(root, rows);

    OpportunityQuery query;
    query.minSize = 1;
    query.olderThanDays = 90;
    query.nowTicks = kNow;
    query.limit = 20;
    const auto expected = oracleFromRows(root, logical, rows, query);
    const auto actual = queryIndexed(root, 20);
    SPACELENS_REQUIRE(actual.report.ok);
    SPACELENS_REQUIRE_EQ(actual.report.opportunities.size(),
                         expected.opportunities.size());
    for (std::size_t i = 0; i < expected.opportunities.size(); ++i) {
        SPACELENS_REQUIRE(actual.report.opportunities[i].path ==
                          expected.opportunities[i].path);
        SPACELENS_REQUIRE(actual.report.opportunities[i].classification ==
                          expected.opportunities[i].classification);
        SPACELENS_REQUIRE(actual.report.opportunities[i].candidateStrength ==
                          expected.opportunities[i].candidateStrength);
        SPACELENS_REQUIRE_EQ(actual.report.opportunities[i].logicalBytes,
                             expected.opportunities[i].logicalBytes);
    }
    SPACELENS_REQUIRE(actual.report.opportunities.front().path.find(L"target") !=
                      std::wstring::npos ||
                      actual.report.opportunities[0].candidateStrength == "Strong" ||
                      actual.report.opportunities[0].candidateStrength == "Moderate");
    removeRootDir(root);
}

SPACELENS_TEST(IndexedIntel_under_and_minsize_before_limit)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("under");
    std::vector<SynthRow> rows;
    for (int i = 0; i < 5000; ++i) {
        rows.push_back(oldLargeFile(L"other\\x-" + std::to_wstring(i) + L".bin",
                                    10ULL * 1024ULL * 1024ULL));
    }
    rows.push_back(hiddenDep(L"keep\\node_modules", 15ULL * 1024ULL * 1024ULL));
    rows.push_back(oldLargeFile(L"keep\\tiny.bin", 100));
    publishSynthetic(root, rows);

    const auto under = queryIndexed(root, 5, {}, joinRoot(root, L"keep"), 1);
    SPACELENS_REQUIRE(under.report.ok);
    SPACELENS_REQUIRE(reportHasPath(under.report, L"keep\\node_modules"));
    for (const auto& item : under.report.opportunities) {
        SPACELENS_REQUIRE(item.path.find(L"\\keep") != std::wstring::npos ||
                          item.path.find(L"\\KEEP") != std::wstring::npos);
        SPACELENS_REQUIRE(item.path.find(L"\\other\\") == std::wstring::npos);
    }

    const auto largeOnly = queryIndexed(root, 5, {}, {}, 14ULL * 1024ULL * 1024ULL);
    SPACELENS_REQUIRE(largeOnly.report.ok);
    SPACELENS_REQUIRE(reportHasPath(largeOnly.report, L"node_modules"));
    removeRootDir(root);
}

SPACELENS_TEST(IndexedIntel_query_under_filter_before_limit)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("query_under");
    std::vector<SynthRow> rows;
    for (int i = 0; i < 5000; ++i) {
        rows.push_back(oldLargeFile(L"noise\\n-" + std::to_wstring(i) + L".bin",
                                    5ULL * 1024ULL * 1024ULL));
    }
    SynthRow inside;
    inside.relPath = L"only\\hit.bin";
    inside.size = 4ULL * 1024ULL * 1024ULL;
    inside.classification = "Unknown";
    inside.strength = "ReviewOnly";
    inside.writeTicks = kNow;
    rows.push_back(inside);
    publishSynthetic(root, rows);

    IndexQuerySpec spec;
    spec.includeFiles = true;
    spec.includeDirectories = false;
    spec.pathPrefix = joinRoot(root, L"only");
    spec.limit = 5;
    spec.sortBy = IndexSortKey::Size;
    const auto q = queryIndex(root, spec);
    SPACELENS_REQUIRE(q.ok);
    SPACELENS_REQUIRE_EQ(q.returned_items, 1ULL);
    SPACELENS_REQUIRE(q.hits.front().path.find(L"hit.bin") != std::wstring::npos);
    removeRootDir(root);
}

SPACELENS_TEST(IndexedIntel_insertion_order_independent)
{
    IsolatedDataRoot data;
    const std::wstring rootA = makeRootDir("order_a");
    const std::wstring rootB = makeRootDir("order_b");
    std::vector<SynthRow> first;
    first.push_back(hiddenDep(L"app\\node_modules", 30ULL * 1024ULL * 1024ULL));
    for (int i = 0; i < 300; ++i) {
        first.push_back(oldLargeFile(L"z-" + std::to_wstring(i) + L".bin",
                                     8ULL * 1024ULL * 1024ULL));
    }
    std::vector<SynthRow> last = first;
    std::swap(last.front(), last.back());
    publishSynthetic(rootA, first);
    publishSynthetic(rootB, last);
    const auto a = queryIndexed(rootA, 5);
    const auto b = queryIndexed(rootB, 5);
    SPACELENS_REQUIRE(a.report.ok);
    SPACELENS_REQUIRE(b.report.ok);
    SPACELENS_REQUIRE_EQ(a.report.opportunities.size(), b.report.opportunities.size());
    for (std::size_t i = 0; i < a.report.opportunities.size(); ++i) {
        SPACELENS_REQUIRE(leafName(a.report.opportunities[i].path) ==
                          leafName(b.report.opportunities[i].path));
        SPACELENS_REQUIRE(a.report.opportunities[i].candidateStrength ==
                          b.report.opportunities[i].candidateStrength);
    }
    removeRootDir(rootA);
    removeRootDir(rootB);
}

SPACELENS_TEST(IndexedIntel_nested_overlap_and_root_bound)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("overlap");
    std::vector<SynthRow> rows;
    SynthRow project = hiddenDep(L"project", 30ULL * 1024ULL * 1024ULL);
    project.classification = "BuildArtifact";
    project.ruleId = "cmake-build-dir";
    project.strength = "Moderate";
    SynthRow nm = hiddenDep(L"project\\node_modules", 20ULL * 1024ULL * 1024ULL);
    SynthRow build = hiddenDep(L"project\\build", 8ULL * 1024ULL * 1024ULL);
    build.classification = "BuildArtifact";
    build.strength = "Moderate";
    rows.push_back(project);
    rows.push_back(nm);
    rows.push_back(build);
    publishSynthetic(root, rows);

    const auto report = queryIndexed(root, 20);
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE(!report.report.uniqueReviewEstimated);
    SPACELENS_REQUIRE(report.report.uniqueReviewBytes <= report.report.logicalBytes);
    SPACELENS_REQUIRE(report.report.uniqueReviewBytes <= 30ULL * 1024ULL * 1024ULL);
    bool sawOverlap = false;
    for (const auto& item : report.report.opportunities) {
        if (item.overlapped) {
            sawOverlap = true;
        }
    }
    SPACELENS_REQUIRE(sawOverlap);
    removeRootDir(root);
}

SPACELENS_TEST(IndexedIntel_unicode_case_and_refresh_rank)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("unicode");
    std::vector<SynthRow> rows;
    for (int i = 0; i < 250; ++i) {
        rows.push_back(oldLargeFile(L"old-" + std::to_wstring(i) + L".bin",
                                    6ULL * 1024ULL * 1024ULL));
    }
    SynthRow uni = hiddenDep(L"项目\\node_modules", 11ULL * 1024ULL * 1024ULL);
    SynthRow mixed = oldLargeFile(L"Mixed Case\\Keep.bin", 7ULL * 1024ULL * 1024ULL);
    SynthRow longName;
    longName.relPath = L"long\\" + std::wstring(180, L'n') + L"\\node_modules";
    longName.directory = true;
    longName.size = 9ULL * 1024ULL * 1024ULL;
    longName.classification = "DependencyDirectory";
    longName.confidence = "High";
    longName.reclaim = "LikelyRegenerable";
    longName.strength = "ReviewOnly";
    longName.writeTicks = kNow;
    rows.push_back(uni);
    rows.push_back(mixed);
    rows.push_back(longName);
    publishSynthetic(root, rows);

    const auto report = queryIndexed(root, 10);
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE(reportHasPath(report.report, L"项目"));

    const auto underMixed =
        queryIndexed(root, 10, {}, joinRoot(root, L"mixed case"), 1);
    SPACELENS_REQUIRE(underMixed.report.ok);
    SPACELENS_REQUIRE(reportHasPath(underMixed.report, L"Keep.bin"));

    auto loc = locateIndex(root);
    {
        auto store = IndexStore::openReadWrite(loc);
        SqliteStmt upd(store.db(),
                       "UPDATE entries SET candidate_strength='Strong', "
                       "confidence='High', classification='BuildArtifact', "
                       "reclaimability='LikelyRegenerable' WHERE name='Keep.bin';");
        upd.stepDone();
    }
    const auto promoted = queryIndexed(root, 5);
    SPACELENS_REQUIRE(reportHasPath(promoted.report, L"Keep.bin"));
    SPACELENS_REQUIRE(promoted.report.opportunities.front().path.find(L"Keep.bin") !=
                          std::wstring::npos ||
                      promoted.report.opportunities[0].candidateStrength == "Strong");

    {
        auto store = IndexStore::openReadWrite(loc);
        SqliteStmt upd(store.db(),
                       "UPDATE entries SET candidate_strength='None', "
                       "classification='Unknown', reclaimability='Unknown' "
                       "WHERE name='Keep.bin';");
        upd.stepDone();
    }
    const auto demoted = queryIndexed(root, 5);
    SPACELENS_REQUIRE(!reportHasPath(demoted.report, L"Keep.bin"));
    removeRootDir(root);
}

SPACELENS_TEST(IndexedIntel_drive_root_under_matches_descendants)
{
    IsolatedDataRoot data;
    // Synthetic drive letter — never created or scanned. Isolated data root
    // keeps this out of the developer AppData catalog.
    const std::wstring root = L"Q:\\";
    std::vector<SynthRow> rows;
    rows.push_back(hiddenDep(L"Users\\late\\node_modules", 12ULL * 1024ULL * 1024ULL));
    rows.push_back(oldLargeFile(L"Users\\decoy.bin", 50ULL * 1024ULL * 1024ULL));
    publishSynthetic(root, rows);

    IndexedOpportunitySpec spec;
    spec.minSize = 1;
    spec.olderThanDays = 90;
    spec.nowTicks = kNow;
    spec.limit = 20;
    spec.pathPrefix = L"Q:\\";
    spec.excludePath = indexedRoot(root);

    const auto fetch = queryIndexedOpportunities(root, spec, {});
    SPACELENS_REQUIRE(fetch.ok);
    SPACELENS_REQUIRE(fetch.error.empty());
    SPACELENS_REQUIRE(fetch.topHits.size() >= 2);
    SPACELENS_REQUIRE(fetch.matchedItems >= 2);

    bool sawNode = false;
    bool sawDecoy = false;
    for (const auto& hit : fetch.topHits) {
        SPACELENS_REQUIRE(hit.path.size() >= 3);
        SPACELENS_REQUIRE((hit.path[0] == L'Q' || hit.path[0] == L'q') &&
                          hit.path[1] == L':');
        if (hit.path.find(L"node_modules") != std::wstring::npos) {
            sawNode = true;
        }
        if (hit.path.find(L"decoy.bin") != std::wstring::npos) {
            sawDecoy = true;
        }
    }
    SPACELENS_REQUIRE(sawNode);
    SPACELENS_REQUIRE(sawDecoy);

    OpportunityQuery query;
    query.minSize = 1;
    query.olderThanDays = 90;
    query.nowTicks = kNow;
    query.limit = 20;
    query.pathPrefix = L"Q:\\";
    const auto expected =
        oracleFromRows(root, 62ULL * 1024ULL * 1024ULL, rows, query);
    SPACELENS_REQUIRE(reportHasPath(expected, L"node_modules"));
    SPACELENS_REQUIRE(reportHasPath(expected, L"decoy.bin"));
    SPACELENS_REQUIRE_EQ(expected.opportunities.size(), 2ULL);
    SPACELENS_REQUIRE(fetch.topHits.size() >= expected.opportunities.size());
    for (std::size_t i = 0; i < expected.opportunities.size(); ++i) {
        SPACELENS_REQUIRE(fetch.topHits[i].path == expected.opportunities[i].path);
    }

    spec.pathPrefix = L"Q:";
    const auto noSlash = queryIndexedOpportunities(root, spec, {});
    SPACELENS_REQUIRE(noSlash.ok);
    SPACELENS_REQUIRE(noSlash.topHits.size() >= 2);

    spec.pathPrefix = L"R:\\";
    const auto otherDrive = queryIndexedOpportunities(root, spec, {});
    SPACELENS_REQUIRE(otherDrive.ok);
    SPACELENS_REQUIRE(otherDrive.topHits.empty());
}

SPACELENS_TEST(IndexedIntel_100k_hidden_candidate_scaling)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("scale100k");
    constexpr int kCount = 100000;
    std::vector<SynthRow> rows;
    rows.reserve(kCount + 1);
    for (int i = 0; i < kCount; ++i) {
        rows.push_back(oldLargeFile(L"bulk\\f-" + std::to_wstring(i) + L".bin",
                                    2ULL * 1024ULL * 1024ULL));
    }
    rows.push_back(hiddenDep(L"tail\\node_modules", 40ULL * 1024ULL * 1024ULL));
    publishSynthetic(root, rows);

    const auto started = std::chrono::steady_clock::now();
    const auto report = queryIndexed(root, 20);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE(reportHasPath(report.report, L"tail\\node_modules"));
    SPACELENS_REQUIRE(report.report.opportunities.size() <= 20);
    SPACELENS_REQUIRE(report.report.toJson().size() < 200000);
    SPACELENS_REQUIRE(!report.report.uniqueReviewEstimated);
    const ByteSize expectedUnique =
        100000ULL * 2ULL * 1024ULL * 1024ULL + 40ULL * 1024ULL * 1024ULL;
    SPACELENS_REQUIRE_EQ(report.report.uniqueReviewBytes, expectedUnique);
    (void)ms;

    std::stop_source cancel;
    cancel.request_stop();
    OpportunityRequest stopped;
    stopped.root = root;
    stopped.fromIndex = true;
    stopped.query.minSize = 1;
    stopped.query.nowTicks = kNow;
    stopped.query.limit = 20;
    const auto cancelled = analyzeOpportunities(stopped, cancel.get_token());
    SPACELENS_REQUIRE(cancelled.error == AnalysisError::Cancelled);
    removeRootDir(root);
}

namespace {

SynthRow regenDir(std::wstring rel, ByteSize size, const char* classification,
                  const char* strength, const char* rule)
{
    SynthRow row;
    row.relPath = std::move(rel);
    row.directory = true;
    row.size = size;
    row.classification = classification;
    row.confidence = "High";
    row.ruleId = rule;
    row.safety = "Ordinary";
    row.reclaim = "LikelyRegenerable";
    row.strength = strength;
    row.writeTicks = kNow - daysToTicks(3);
    return row;
}

void expectSameAggregates(const OpportunityReport& production,
                          const OpportunityReport& oracle)
{
    SPACELENS_REQUIRE_EQ(production.uniqueReviewBytes, oracle.uniqueReviewBytes);
    SPACELENS_REQUIRE(production.uniqueReviewEstimated ==
                      oracle.uniqueReviewEstimated);
    SPACELENS_REQUIRE_EQ(production.groups.size(), oracle.groups.size());
    for (std::size_t i = 0; i < production.groups.size(); ++i) {
        SPACELENS_REQUIRE(production.groups[i].id == oracle.groups[i].id);
        SPACELENS_REQUIRE_EQ(production.groups[i].logicalBytes,
                             oracle.groups[i].logicalBytes);
        SPACELENS_REQUIRE_EQ(production.groups[i].itemCount,
                             oracle.groups[i].itemCount);
        SPACELENS_REQUIRE(production.groups[i].strongestCandidateStrength ==
                          oracle.groups[i].strongestCandidateStrength);
    }
}

OpportunityQuery defaultAggQuery(std::size_t limit = 20,
                                 std::optional<StorageCategory> cls = {},
                                 std::wstring under = {}, ByteSize minSize = 1)
{
    OpportunityQuery query;
    query.minSize = minSize;
    query.olderThanDays = 90;
    query.nowTicks = kNow;
    query.limit = limit;
    query.categoryOnly = cls;
    query.pathPrefix = std::move(under);
    return query;
}

}  // namespace

SPACELENS_TEST(ExactAgg_adversarial_nesting_and_sibling)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("nest");
    std::vector<SynthRow> rows;
    rows.push_back(regenDir(L"A", 100, "BuildArtifact", "Moderate", "cmake-build-dir"));
    rows.push_back(regenDir(L"A\\child1", 60, "BuildArtifact", "Moderate",
                            "cmake-build-dir"));
    rows.push_back(regenDir(L"A\\child2", 30, "BuildArtifact", "Moderate",
                            "cmake-build-dir"));
    rows.push_back(regenDir(L"A\\child2\\grandchild", 20, "BuildArtifact",
                            "Moderate", "cmake-build-dir"));
    rows.push_back(regenDir(L"B", 50, "DependencyDirectory", "Strong",
                            "node-modules"));
    publishSynthetic(root, rows);

    const auto report = queryIndexed(root, 20);
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE(!report.report.uniqueReviewEstimated);
    SPACELENS_REQUIRE_EQ(report.report.uniqueReviewBytes, 150ULL);
    expectSameAggregates(report.report,
                         oracleFromRows(root, 260, rows, defaultAggQuery()));
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_near_prefix_not_descendant)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("near");
    std::vector<SynthRow> rows;
    rows.push_back(regenDir(L"foo", 100, "BuildArtifact", "Moderate",
                            "cmake-build-dir"));
    rows.push_back(regenDir(L"foo\\bar", 60, "BuildArtifact", "Moderate",
                            "cmake-build-dir"));
    rows.push_back(regenDir(L"foobar", 50, "DependencyDirectory", "Strong",
                            "node-modules"));
    rows.push_back(regenDir(L"foo2", 40, "PackageCache", "Strong",
                            "package-cache-name"));
    publishSynthetic(root, rows);

    const auto report = queryIndexed(root, 20);
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE_EQ(report.report.uniqueReviewBytes, 190ULL);
    expectSameAggregates(report.report,
                         oracleFromRows(root, 250, rows, defaultAggQuery()));
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_cross_category_group_semantics)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("xcat");
    std::vector<SynthRow> rows;
    rows.push_back(regenDir(L"Project", 20ULL * 1024ULL * 1024ULL, "BuildArtifact",
                            "Moderate", "cmake-build-dir"));
    rows.push_back(hiddenDep(L"Project\\node_modules", 12ULL * 1024ULL * 1024ULL));
    rows.push_back(regenDir(L"Project\\.cache", 5ULL * 1024ULL * 1024ULL,
                            "PackageCache", "Strong", "package-cache-name"));
    publishSynthetic(root, rows);

    const auto report = queryIndexed(root, 20);
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE_EQ(report.report.uniqueReviewBytes, 20ULL * 1024ULL * 1024ULL);
    ByteSize groupSum = 0;
    bool sawBuild = false;
    bool sawDep = false;
    bool sawPkg = false;
    for (const auto& group : report.report.groups) {
        groupSum += group.logicalBytes;
        if (group.id == "generated_outputs") {
            sawBuild = true;
            SPACELENS_REQUIRE_EQ(group.logicalBytes, 20ULL * 1024ULL * 1024ULL);
            SPACELENS_REQUIRE_EQ(group.itemCount, 1ULL);
        } else if (group.id == "developer_dependencies") {
            sawDep = true;
            SPACELENS_REQUIRE_EQ(group.logicalBytes, 0ULL);
            SPACELENS_REQUIRE_EQ(group.itemCount, 1ULL);
        } else if (group.id == "package_cache") {
            sawPkg = true;
            SPACELENS_REQUIRE_EQ(group.logicalBytes, 0ULL);
            SPACELENS_REQUIRE_EQ(group.itemCount, 1ULL);
        }
    }
    SPACELENS_REQUIRE(sawBuild && sawDep && sawPkg);
    SPACELENS_REQUIRE_EQ(groupSum, report.report.uniqueReviewBytes);
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_mixed_non_opportunity_parent)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("mixedp");
    std::vector<SynthRow> rows;
    SynthRow parent;
    parent.relPath = L"Projects";
    parent.directory = true;
    parent.size = 40ULL * 1024ULL * 1024ULL;
    parent.classification = "Unknown";
    parent.confidence = "Low";
    parent.strength = "None";
    parent.reclaim = "Unknown";
    rows.push_back(parent);
    rows.push_back(hiddenDep(L"Projects\\App\\node_modules", 20ULL * 1024ULL * 1024ULL));
    rows.push_back(regenDir(L"Projects\\App\\build", 10ULL * 1024ULL * 1024ULL,
                            "BuildArtifact", "Moderate", "cmake-build-dir"));
    publishSynthetic(root, rows);

    const auto report = queryIndexed(root, 20);
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE_EQ(report.report.uniqueReviewBytes, 30ULL * 1024ULL * 1024ULL);
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_file_under_directory_opportunity)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("filecov");
    std::vector<SynthRow> rows;
    rows.push_back(regenDir(L"cache", 100, "TemporaryData", "Strong",
                            "known-temp-folder"));
    rows.push_back(oldLargeFile(L"cache\\old.zip", 10));
    rows.push_back(oldLargeFile(L"sibling.bin", 7));
    publishSynthetic(root, rows);

    const auto report = queryIndexed(root, 20);
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE_EQ(report.report.uniqueReviewBytes, 107ULL);
    bool zipOverlapped = false;
    for (const auto& item : report.report.opportunities) {
        if (item.path.find(L"old.zip") != std::wstring::npos) {
            zipOverlapped = item.overlapped;
        }
    }
    SPACELENS_REQUIRE(zipOverlapped);
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_classification_and_under_filters)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("filters");
    std::vector<SynthRow> rows;
    for (int i = 0; i < 80; ++i) {
        rows.push_back(oldLargeFile(L"bulk\\f-" + std::to_wstring(i) + L".bin", 8));
    }
    rows.push_back(hiddenDep(L"keep\\node_modules", 40));
    rows.push_back(regenDir(L"keep\\.cache", 9, "PackageCache", "Strong",
                            "package-cache-name"));
    publishSynthetic(root, rows);

    const auto pkg = queryIndexed(root, 20, StorageCategory::PackageCache);
    SPACELENS_REQUIRE(pkg.report.ok);
    SPACELENS_REQUIRE_EQ(pkg.report.uniqueReviewBytes, 9ULL);
    SPACELENS_REQUIRE_EQ(pkg.report.groups.size(), 1ULL);
    SPACELENS_REQUIRE(pkg.report.groups[0].id == "package_cache");

    const auto under =
        queryIndexed(root, 20, {}, joinRoot(root, L"keep"), 1);
    SPACELENS_REQUIRE(under.report.ok);
    SPACELENS_REQUIRE_EQ(under.report.uniqueReviewBytes, 49ULL);
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_unicode_under_and_mcp_utf8)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("uniagg");
    std::vector<SynthRow> rows;
    rows.push_back(hiddenDep(L"tài liệu\\node_modules", 15));
    rows.push_back(oldLargeFile(L"tài liệu\\cũ.bin", 8));
    rows.push_back(oldLargeFile(L"other\\x.bin", 4));
    publishSynthetic(root, rows);

    const std::wstring underWide = joinRoot(root, L"tài liệu");
    const auto report = queryIndexed(root, 20, {}, underWide, 1);
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE_EQ(report.report.uniqueReviewBytes, 23ULL);
    SPACELENS_REQUIRE(reportHasPath(report.report, L"tài liệu"));

    const std::string utf8 = utf8FromWide(underWide);
    const std::wstring roundTrip = wideFromUtf8(utf8);
    SPACELENS_REQUIRE(normalizeOpportunityPathKey(roundTrip) ==
                      normalizeOpportunityPathKey(underWide));

    IndexedOpportunitySpec spec;
    spec.minSize = 1;
    spec.olderThanDays = 90;
    spec.nowTicks = kNow;
    spec.limit = 20;
    spec.pathPrefix = wideFromUtf8(utf8);
    spec.excludePath = indexedRoot(root);
    const auto fetch = queryIndexedOpportunities(root, spec, {});
    SPACELENS_REQUIRE(fetch.ok);
    SPACELENS_REQUIRE_EQ(fetch.uniqueReviewBytes, 23ULL);
    SPACELENS_REQUIRE(!fetch.uniqueReviewEstimated);
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_overflow_saturates_not_wraps)
{
    OpportunityStreamReducer reducer;
    OpportunityAggregateInput a;
    a.path = L"D:\\a.bin";
    a.logicalBytes = std::numeric_limits<ByteSize>::max();
    a.oldLargeFile = true;
    a.candidateStrength = "ReviewOnly";
    a.classification = "Unknown";
    reducer.observe(std::move(a));
    OpportunityAggregateInput b;
    b.path = L"D:\\b.bin";
    b.logicalBytes = 1;
    b.oldLargeFile = true;
    b.candidateStrength = "ReviewOnly";
    b.classification = "Unknown";
    reducer.observe(std::move(b));
    SPACELENS_REQUIRE(reducer.overflow());
    SPACELENS_REQUIRE(reducer.uniqueReviewEstimated());
    SPACELENS_REQUIRE_EQ(reducer.uniqueReviewBytes(),
                         std::numeric_limits<ByteSize>::max());
}

SPACELENS_TEST(ExactAgg_duplicate_path_not_double_counted)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("duppath");
    std::vector<SynthRow> rows;
    rows.push_back(hiddenDep(L"app\\node_modules", 12));
    publishSynthetic(root, rows);
    {
        auto loc = locateIndex(root);
        auto store = IndexStore::openReadWrite(loc);
        SqliteStmt insert(
            store.db(),
            "INSERT INTO entries("
            "id, root_id, parent_id, kind, name, path, size_bytes, recursive_size, "
            "extension, last_write_ticks, last_access_ticks, attributes, is_reparse, "
            "classification, confidence, rule_id, location_safety, reclaimability, "
            "candidate_strength, newest_descendant_write, oldest_descendant_write, "
            "file_id, parent_file_id) "
            "VALUES(99,1,NULL,1,'node_modules',?1,12,12,'',0,0,0,0,"
            "'DependencyDirectory','High','node-modules','Ordinary',"
            "'LikelyRegenerable','Strong',0,0,0,0);");
        insert.bindText16(1, joinRoot(indexedRoot(root), L"app\\node_modules"));
        insert.stepDone();
    }
    const auto report = queryIndexed(root, 20);
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE_EQ(report.report.uniqueReviewBytes, 12ULL);
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_above_historical_50k_threshold)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("over50k");
    constexpr int kCount = 50001;
    std::vector<SynthRow> rows;
    rows.reserve(static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        rows.push_back(oldLargeFile(L"bulk\\f-" + std::to_wstring(i) + L".bin", 3));
    }
    publishSynthetic(root, rows);

    const auto report = queryIndexed(root, 20);
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE(!report.report.uniqueReviewEstimated);
    SPACELENS_REQUIRE_EQ(report.report.uniqueReviewBytes,
                         static_cast<ByteSize>(kCount) * 3ULL);
    SPACELENS_REQUIRE(report.report.toJson().size() < 200000);
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_mid_query_cancel_not_exact)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("midcancel");
    std::vector<SynthRow> rows;
    rows.reserve(4000);
    for (int i = 0; i < 4000; ++i) {
        rows.push_back(oldLargeFile(L"bulk\\f-" + std::to_wstring(i) + L".bin", 2));
    }
    publishSynthetic(root, rows);

    IndexedOpportunitySpec spec;
    spec.minSize = 1;
    spec.olderThanDays = 90;
    spec.nowTicks = kNow;
    spec.limit = 20;
    spec.excludePath = indexedRoot(root);
    spec.cancelAfterStreamedRows = 250;
    const auto fetch = queryIndexedOpportunities(root, spec, {});
    SPACELENS_REQUIRE(!fetch.ok);
    SPACELENS_REQUIRE(fetch.error == "cancelled");
    SPACELENS_REQUIRE(fetch.uniqueReviewBytes == 0);
    SPACELENS_REQUIRE(!fetch.uniqueReviewEstimated);
    SPACELENS_REQUIRE(!fetch.aggregateOverflow);
    SPACELENS_REQUIRE(fetch.groups.empty());
    SPACELENS_REQUIRE(fetch.rowsStreamed >= spec.cancelAfterStreamedRows);
    SPACELENS_REQUIRE(fetch.rowsStreamed < 4000);
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_randomized_differential)
{
    IsolatedDataRoot data;
    const std::uint32_t seeds[] = {1u, 2u, 3u, 7u, 42u};
    for (std::uint32_t seed : seeds) {
        const std::wstring root =
            makeRootDir(("rand" + std::to_string(seed)).c_str());
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> depthDist(1, 4);
        std::uniform_int_distribution<int> kindDist(0, 5);
        std::uniform_int_distribution<int> sizeDist(2, 40);
        std::vector<std::wstring> coveringDirs;
        std::vector<SynthRow> rows;
        rows.reserve(180);
        for (int i = 0; i < 180; ++i) {
            std::wstring rel;
            if (!coveringDirs.empty() && (i % 4 == 0)) {
                rel = coveringDirs[static_cast<std::size_t>(i) % coveringDirs.size()];
                rel += L"\\c";
                rel += std::to_wstring(i);
            } else {
                const int depth = depthDist(rng);
                for (int d = 0; d < depth; ++d) {
                    if (!rel.empty()) {
                        rel += L'\\';
                    }
                    rel += L"n";
                    rel += std::to_wstring((i + d * 17) % 40);
                }
                rel += L"\\i";
                rel += std::to_wstring(i);
            }
            const int kind = kindDist(rng);
            SynthRow made;
            if (kind == 0) {
                made = hiddenDep(rel + L"\\node_modules",
                                 static_cast<ByteSize>(sizeDist(rng)));
            } else if (kind == 1) {
                made = regenDir(rel + L"\\build",
                                static_cast<ByteSize>(sizeDist(rng)),
                                "BuildArtifact", "Moderate",
                                "cmake-build-dir");
            } else if (kind == 2) {
                made = regenDir(rel + L"\\.cache",
                                static_cast<ByteSize>(sizeDist(rng)),
                                "PackageCache", "Strong",
                                "package-cache-name");
            } else if (kind == 3) {
                made = hiddenDep(rel + L"\\protected", 9);
                made.safety = "Protected";
                made.strength = "None";
            } else {
                made = oldLargeFile(rel + L"\\old.bin",
                                    static_cast<ByteSize>(sizeDist(rng)));
            }
            rows.push_back(made);
            if (made.directory && made.safety != "Protected" &&
                made.strength != "None") {
                coveringDirs.push_back(made.relPath);
            }
        }
        publishSynthetic(root, rows);
        const auto query = defaultAggQuery(20);
        const auto production = queryIndexed(root, 20);
        const auto oracle = oracleFromRows(root, 1, rows, query);
        SPACELENS_REQUIRE(production.report.ok);
        expectSameAggregates(production.report, oracle);
        removeRootDir(root);
    }
}

#ifdef NDEBUG
SPACELENS_TEST(ExactAgg_500k_exact_release)
{
    IsolatedDataRoot data;
    const std::wstring root = makeRootDir("scale500k");
    constexpr int kCount = 500000;
    std::vector<SynthRow> rows;
    rows.reserve(static_cast<std::size_t>(kCount) + 1);
    for (int i = 0; i < kCount; ++i) {
        rows.push_back(oldLargeFile(L"bulk\\f-" + std::to_wstring(i) + L".bin", 2));
    }
    rows.push_back(hiddenDep(L"tail\\node_modules", 40));
    publishSynthetic(root, rows);

    const auto started = std::chrono::steady_clock::now();
    const auto report = queryIndexed(root, 20);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    SPACELENS_REQUIRE(report.report.ok);
    SPACELENS_REQUIRE(!report.report.uniqueReviewEstimated);
    SPACELENS_REQUIRE_EQ(report.report.uniqueReviewBytes,
                         static_cast<ByteSize>(kCount) * 2ULL + 40ULL);
    SPACELENS_REQUIRE(reportHasPath(report.report, L"tail\\node_modules"));
    SPACELENS_REQUIRE(report.report.toJson().size() < 200000);
    std::cout << "[ TIME ] ExactAgg_500k query_ms=" << ms << '\n';
    removeRootDir(root);
}

SPACELENS_TEST(ExactAgg_scale_measurements)
{
    if (std::getenv("SPACELENS_BENCH_AGG") == nullptr) {
        return;
    }
    IsolatedDataRoot data;
    const int sizes[] = {10000, 50000, 100000, 250000, 500000, 1000000};
    for (int n : sizes) {
        const std::wstring root =
            makeRootDir(("bench" + std::to_string(n)).c_str());
        std::vector<SynthRow> rows;
        rows.reserve(static_cast<std::size_t>(n) + 1);
        for (int i = 0; i < n; ++i) {
            rows.push_back(oldLargeFile(L"bulk\\f-" + std::to_wstring(i) + L".bin", 2));
        }
        rows.push_back(hiddenDep(L"tail\\node_modules", 40));
        const auto pub0 = std::chrono::steady_clock::now();
        publishSynthetic(root, rows);
        const auto pubMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - pub0)
                               .count();

        IndexedOpportunitySpec spec;
        spec.minSize = 1;
        spec.olderThanDays = 90;
        spec.nowTicks = kNow;
        spec.limit = 20;
        spec.excludePath = indexedRoot(root);
        const auto q0 = std::chrono::steady_clock::now();
        const auto fetch = queryIndexedOpportunities(root, spec, {});
        const auto qMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - q0)
                             .count();
        std::cout << "[ BENCH ] n=" << n << " publish_ms=" << pubMs
                  << " query_ms=" << qMs
                  << " unique=" << fetch.uniqueReviewBytes
                  << " estimated=" << (fetch.uniqueReviewEstimated ? "true" : "false")
                  << " streamed=" << fetch.rowsStreamed
                  << " depth=" << fetch.maxActiveDepth << '\n';
        SPACELENS_REQUIRE(fetch.ok);
        SPACELENS_REQUIRE(!fetch.uniqueReviewEstimated);
        SPACELENS_REQUIRE_EQ(fetch.uniqueReviewBytes,
                             static_cast<ByteSize>(n) * 2ULL + 40ULL);
        removeRootDir(root);
    }
}
#endif
