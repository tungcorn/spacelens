#include "TestRunner.hpp"

#include "core/FileTime.hpp"
#include "core/StorageAnalysis.hpp"
#include "core/index/IndexBreakdown.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexStore.hpp"
#include "core/index/Sqlite.hpp"

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
#include <limits>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

using namespace spacelens;

namespace {

constexpr FileTimeTicks kNow = 133800000000000000ULL;

struct BreakdownRow {
    std::wstring relPath;
    bool directory = false;
    ByteSize size = 0;
    ByteSize recursive_size = 0;
    std::string extension;
    std::string classification = "Unknown";
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
        m_dir = fs::temp_directory_path() / "spacelens_idx_breakdown_appdata" /
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

void publishBreakdown(const std::wstring& root,
                      const std::vector<BreakdownRow>& rows,
                      FileTimeTicks indexedAt = kNow)
{
    auto loc = locateIndex(root);
    ByteSize fileLogical = 0;
    std::uint64_t files = 0;
    std::uint64_t dirs = 1;
    for (const auto& row : rows) {
        if (row.directory) {
            ++dirs;
        } else {
            ++files;
            fileLogical += row.size;
        }
    }

    {
        auto store = IndexStore::createStaging(loc);
        IndexRootInfo meta;
        meta.rootId = 1;
        meta.rootPath = loc.rootPath;
        meta.rootKey = loc.rootKey;
        meta.schemaVersion = kIndexSchemaVersion;
        meta.indexedAtTicks = indexedAt;
        meta.indexedAtIso = fileTimeTicksToIsoUtc(indexedAt);
        meta.fileCount = files;
        meta.dirCount = dirs;
        meta.logicalBytes = fileLogical;
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
        insert.bindText16(5, leafName(loc.rootPath));
        insert.bindText16(6, loc.rootPath);
        insert.bindInt64(7, static_cast<std::int64_t>(fileLogical));
        insert.bindInt64(8, static_cast<std::int64_t>(fileLogical * 10 + 1));
        insert.bindText(9, "");
        insert.bindInt64(10, static_cast<std::int64_t>(indexedAt));
        insert.bindInt64(11, 0);
        insert.bindInt64(12, 0);
        insert.bindInt64(13, 0);
        insert.bindText(14, "Unknown");
        insert.bindText(15, "Low");
        insert.bindText(16, "");
        insert.bindText(17, "Ordinary");
        insert.bindText(18, "Unknown");
        insert.bindText(19, "None");
        insert.bindInt64(20, static_cast<std::int64_t>(indexedAt));
        insert.bindInt64(21, 0);
        insert.bindInt64(22, 0);
        insert.bindInt64(23, 0);
        insert.stepDone();

        for (const auto& row : rows) {
            const std::wstring path = joinRoot(loc.rootPath, row.relPath);
            const ByteSize rec =
                row.recursive_size != 0 ? row.recursive_size : row.size;
            insert.reset();
            insert.clearBindings();
            insert.bindInt64(1, id++);
            insert.bindInt64(2, 1);
            insert.bindNull(3);
            insert.bindInt64(4, row.directory ? 1 : 0);
            insert.bindText16(5, leafName(row.relPath));
            insert.bindText16(6, path);
            insert.bindInt64(7, static_cast<std::int64_t>(row.size));
            insert.bindInt64(8, static_cast<std::int64_t>(rec));
            insert.bindText(9, row.extension);
            insert.bindInt64(10, static_cast<std::int64_t>(row.writeTicks));
            insert.bindInt64(11, 0);
            insert.bindInt64(12, 0);
            insert.bindInt64(13, 0);
            insert.bindText(14, row.classification);
            insert.bindText(15, "High");
            insert.bindText(16, "");
            insert.bindText(17, "Ordinary");
            insert.bindText(18, "Unknown");
            insert.bindText(19, "None");
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

BreakdownRow fileRow(std::wstring rel, ByteSize size, std::string ext,
                     std::string classification, FileTimeTicks write)
{
    BreakdownRow row;
    row.relPath = std::move(rel);
    row.size = size;
    row.extension = std::move(ext);
    row.classification = std::move(classification);
    row.writeTicks = write;
    return row;
}

BreakdownRow dirRow(std::wstring rel, ByteSize recursive)
{
    BreakdownRow row;
    row.relPath = std::move(rel);
    row.directory = true;
    row.size = 50;
    row.recursive_size = recursive;
    row.classification = "Unknown";
    row.writeTicks = kNow;
    return row;
}

void requireReconcile(const IndexedBreakdown& r)
{
    SPACELENS_REQUIRE(r.ok);
    SPACELENS_REQUIRE(r.error.empty());
    std::uint64_t classFiles = 0;
    ByteSize classBytes = 0;
    for (const auto& g : r.by_classification) {
        classFiles += g.file_count;
        classBytes += g.logical_bytes;
    }
    std::uint64_t extFiles = r.extension_other.file_count;
    ByteSize extBytes = r.extension_other.logical_bytes;
    for (const auto& g : r.by_extension) {
        extFiles += g.file_count;
        extBytes += g.logical_bytes;
    }
    std::uint64_t ageFiles = 0;
    ByteSize ageBytes = 0;
    SPACELENS_REQUIRE_EQ(r.by_last_write_age.size(), 6ULL);
    for (const auto& g : r.by_last_write_age) {
        ageFiles += g.file_count;
        ageBytes += g.logical_bytes;
    }
    SPACELENS_REQUIRE_EQ(classFiles, r.total_file_count);
    SPACELENS_REQUIRE_EQ(extFiles, r.total_file_count);
    SPACELENS_REQUIRE_EQ(ageFiles, r.total_file_count);
    SPACELENS_REQUIRE_EQ(classBytes, r.total_logical_bytes);
    SPACELENS_REQUIRE_EQ(extBytes, r.total_logical_bytes);
    SPACELENS_REQUIRE_EQ(ageBytes, r.total_logical_bytes);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[0].key, std::string("lt_30d"));
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[1].key, std::string("d30_90"));
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[2].key, std::string("d90_365"));
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[3].key, std::string("ge_365d"));
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[4].key, std::string("unknown"));
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[5].key, std::string("future"));
}

const BreakdownGroup* findGroup(const std::vector<BreakdownGroup>& groups,
                                std::string_view key)
{
    for (const auto& g : groups) {
        if (g.key == key) {
            return &g;
        }
    }
    return nullptr;
}

IndexedBreakdown runBreakdown(const std::wstring& root,
                              std::wstring under = {},
                              std::size_t limit = 20,
                              std::optional<std::uint64_t> maxAge = {},
                              FileTimeTicks now = kNow)
{
    IndexedBreakdownSpec spec;
    spec.pathPrefix = std::move(under);
    spec.limit = limit;
    spec.nowTicks = now;
    spec.maxIndexAgeSeconds = maxAge;
    return queryIndexedBreakdown(root, spec);
}

}  // namespace

SPACELENS_TEST(Breakdown_age_bucket_boundaries)
{
    SPACELENS_REQUIRE(classifyWriteAgeBucket(0, kNow) == WriteAgeBucket::Unknown);
    SPACELENS_REQUIRE(classifyWriteAgeBucket(kNow + 1, kNow) ==
                      WriteAgeBucket::Future);
    SPACELENS_REQUIRE(classifyWriteAgeBucket(kNow, 0) == WriteAgeBucket::Future);
    SPACELENS_REQUIRE(classifyWriteAgeBucket(kNow, kNow) == WriteAgeBucket::Lt30d);
    SPACELENS_REQUIRE(classifyWriteAgeBucket(kNow - daysToTicks(29), kNow) ==
                      WriteAgeBucket::Lt30d);
    SPACELENS_REQUIRE(classifyWriteAgeBucket(kNow - daysToTicks(30), kNow) ==
                      WriteAgeBucket::D30_90);
    SPACELENS_REQUIRE(classifyWriteAgeBucket(kNow - daysToTicks(89), kNow) ==
                      WriteAgeBucket::D30_90);
    SPACELENS_REQUIRE(classifyWriteAgeBucket(kNow - daysToTicks(90), kNow) ==
                      WriteAgeBucket::D90_365);
    SPACELENS_REQUIRE(classifyWriteAgeBucket(kNow - daysToTicks(364), kNow) ==
                      WriteAgeBucket::D90_365);
    SPACELENS_REQUIRE(classifyWriteAgeBucket(kNow - daysToTicks(365), kNow) ==
                      WriteAgeBucket::Ge365d);
    SPACELENS_REQUIRE_EQ(std::string(toString(WriteAgeBucket::Lt30d)),
                         std::string("lt_30d"));
}

SPACELENS_TEST(Breakdown_reconciles_and_ignores_directory_recursive_size)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    std::vector<BreakdownRow> rows;
    rows.push_back(dirRow(L"huge", 9ULL * 1024ULL * 1024ULL * 1024ULL));
    rows.push_back(fileRow(L"huge\\a.bin", 40, "bin", "Media",
                           kNow - daysToTicks(10)));
    rows.push_back(fileRow(L"huge\\b.log", 60, "log", "Logs",
                           kNow - daysToTicks(40)));
    rows.push_back(fileRow(L"README", 10, "", "UserData", kNow - daysToTicks(200)));
    publishBreakdown(root, rows);

    const auto r = runBreakdown(root);
    requireReconcile(r);
    SPACELENS_REQUIRE_EQ(r.total_file_count, 3ULL);
    SPACELENS_REQUIRE_EQ(r.total_logical_bytes, 110ULL);
    SPACELENS_REQUIRE(!r.logical_bytes_estimated);
    SPACELENS_REQUIRE(r.total_logical_bytes < 1024ULL * 1024ULL);
    const auto* media = findGroup(r.by_classification, "Media");
    const auto* logs = findGroup(r.by_classification, "Logs");
    const auto* user = findGroup(r.by_classification, "UserData");
    SPACELENS_REQUIRE(media && logs && user);
    SPACELENS_REQUIRE_EQ(media->logical_bytes, 40ULL);
    SPACELENS_REQUIRE_EQ(logs->logical_bytes, 60ULL);
    SPACELENS_REQUIRE_EQ(user->logical_bytes, 10ULL);
    const auto* none = findGroup(r.by_extension, "");
    SPACELENS_REQUIRE(none);
    SPACELENS_REQUIRE_EQ(none->file_count, 1ULL);
    SPACELENS_REQUIRE_EQ(none->logical_bytes, 10ULL);
}

SPACELENS_TEST(Breakdown_extension_topn_plus_other_exact)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    std::vector<BreakdownRow> rows;
    rows.push_back(fileRow(L"a.bin", 100, "bin", "Unknown", kNow));
    rows.push_back(fileRow(L"b.dll", 90, "dll", "Unknown", kNow));
    rows.push_back(fileRow(L"c.exe", 80, "exe", "Unknown", kNow));
    rows.push_back(fileRow(L"d.dat", 70, "dat", "Unknown", kNow));
    rows.push_back(fileRow(L"e.tmp", 60, "tmp", "Unknown", kNow));
    rows.push_back(fileRow(L"f.bak", 50, "bak", "Unknown", kNow));
    rows.push_back(fileRow(L"g.old", 40, "old", "Unknown", kNow));
    publishBreakdown(root, rows);

    const auto r = runBreakdown(root, {}, 3);
    requireReconcile(r);
    SPACELENS_REQUIRE_EQ(r.limit, 3ULL);
    SPACELENS_REQUIRE_EQ(r.by_extension.size(), 3ULL);
    SPACELENS_REQUIRE_EQ(r.by_extension[0].key, std::string("bin"));
    SPACELENS_REQUIRE_EQ(r.by_extension[1].key, std::string("dll"));
    SPACELENS_REQUIRE_EQ(r.by_extension[2].key, std::string("exe"));
    SPACELENS_REQUIRE_EQ(r.extension_other.extension_groups, 4ULL);
    SPACELENS_REQUIRE_EQ(r.extension_other.file_count, 4ULL);
    SPACELENS_REQUIRE_EQ(r.extension_other.logical_bytes, 220ULL);
    SPACELENS_REQUIRE_EQ(r.total_logical_bytes, 490ULL);
}

SPACELENS_TEST(Breakdown_classification_and_tie_order)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    std::vector<BreakdownRow> rows;
    rows.push_back(fileRow(L"m1.bin", 50, "bin", "Media", kNow));
    rows.push_back(fileRow(L"l1.log", 50, "log", "Logs", kNow));
    rows.push_back(fileRow(L"b1.o", 80, "o", "BuildArtifact", kNow));
    publishBreakdown(root, rows);

    const auto r = runBreakdown(root);
    requireReconcile(r);
    SPACELENS_REQUIRE_EQ(r.by_classification.size(), 3ULL);
    SPACELENS_REQUIRE_EQ(r.by_classification[0].key, std::string("BuildArtifact"));
    SPACELENS_REQUIRE_EQ(r.by_classification[1].key, std::string("Logs"));
    SPACELENS_REQUIRE_EQ(r.by_classification[2].key, std::string("Media"));
}

SPACELENS_TEST(Breakdown_age_sql_matches_classifier)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    std::vector<BreakdownRow> rows;
    rows.push_back(fileRow(L"recent.bin", 1, "bin", "Unknown",
                           kNow - daysToTicks(29)));
    rows.push_back(fileRow(L"exact30.bin", 2, "bin", "Unknown",
                           kNow - daysToTicks(30)));
    rows.push_back(fileRow(L"exact90.bin", 3, "bin", "Unknown",
                           kNow - daysToTicks(90)));
    rows.push_back(fileRow(L"exact365.bin", 4, "bin", "Unknown",
                           kNow - daysToTicks(365)));
    rows.push_back(fileRow(L"unknown.bin", 5, "bin", "Unknown", 0));
    rows.push_back(fileRow(L"future.bin", 6, "bin", "Unknown",
                           kNow + daysToTicks(1)));
    publishBreakdown(root, rows);

    const auto r = runBreakdown(root);
    requireReconcile(r);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[0].file_count, 1ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[0].logical_bytes, 1ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[1].file_count, 1ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[1].logical_bytes, 2ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[2].file_count, 1ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[2].logical_bytes, 3ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[3].file_count, 1ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[3].logical_bytes, 4ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[4].file_count, 1ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[4].logical_bytes, 5ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[5].file_count, 1ULL);
    SPACELENS_REQUIRE_EQ(r.by_last_write_age[5].logical_bytes, 6ULL);
}

SPACELENS_TEST(Breakdown_under_drive_root_and_sibling_trap)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    std::vector<BreakdownRow> rows;
    rows.push_back(fileRow(L"foo\\a.bin", 10, "bin", "Unknown", kNow));
    rows.push_back(fileRow(L"foo\\c.bin", 5, "bin", "Unknown", kNow));
    rows.push_back(fileRow(L"foobar\\b.bin", 20, "bin", "Unknown", kNow));
    publishBreakdown(root, rows);

    const auto whole = runBreakdown(root, L"Q:\\");
    requireReconcile(whole);
    SPACELENS_REQUIRE_EQ(whole.total_file_count, 3ULL);
    SPACELENS_REQUIRE_EQ(whole.total_logical_bytes, 35ULL);

    const auto noSlash = runBreakdown(root, L"Q:");
    requireReconcile(noSlash);
    SPACELENS_REQUIRE_EQ(noSlash.total_file_count, 3ULL);

    const auto foo = runBreakdown(root, L"Q:\\foo");
    requireReconcile(foo);
    SPACELENS_REQUIRE_EQ(foo.total_file_count, 2ULL);
    SPACELENS_REQUIRE_EQ(foo.total_logical_bytes, 15ULL);

    const auto other = runBreakdown(root, L"R:\\");
    requireReconcile(other);
    SPACELENS_REQUIRE_EQ(other.total_file_count, 0ULL);
    SPACELENS_REQUIRE_EQ(other.total_logical_bytes, 0ULL);
}

SPACELENS_TEST(Breakdown_stale_max_age_fails_closed)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    std::vector<BreakdownRow> rows;
    rows.push_back(fileRow(L"a.bin", 10, "bin", "Unknown", kNow));
    publishBreakdown(root, rows, kNow - daysToTicks(2));

    const auto stale = runBreakdown(root, {}, 20, 60ULL, kNow);
    SPACELENS_REQUIRE(!stale.ok);
    SPACELENS_REQUIRE_EQ(stale.error, std::string("index_too_old"));
    SPACELENS_REQUIRE(stale.by_classification.empty());
    SPACELENS_REQUIRE(stale.by_extension.empty());
    SPACELENS_REQUIRE_EQ(stale.total_file_count, 0ULL);

    const auto fresh = runBreakdown(root, {}, 20, 10ULL * 24ULL * 3600ULL, kNow);
    requireReconcile(fresh);
    SPACELENS_REQUIRE_EQ(fresh.total_file_count, 1ULL);
}

SPACELENS_TEST(Breakdown_unknown_freshness_fails_closed)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    std::vector<BreakdownRow> rows;
    rows.push_back(fileRow(L"a.bin", 10, "bin", "Unknown", kNow));
    publishBreakdown(root, rows, 0);

    const auto unknown = runBreakdown(root, {}, 20, 60ULL, kNow);
    SPACELENS_REQUIRE(!unknown.ok);
    SPACELENS_REQUIRE_EQ(unknown.error, std::string("index_freshness_unknown"));
    SPACELENS_REQUIRE(unknown.by_classification.empty());
}

SPACELENS_TEST(Breakdown_empty_index_and_empty_subtree)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    std::vector<BreakdownRow> rows;
    rows.push_back(fileRow(L"keep\\a.bin", 10, "bin", "Unknown", kNow));
    publishBreakdown(root, rows);

    const auto empty = runBreakdown(root, L"Q:\\missing");
    requireReconcile(empty);
    SPACELENS_REQUIRE_EQ(empty.total_file_count, 0ULL);
    SPACELENS_REQUIRE(empty.by_classification.empty());
    SPACELENS_REQUIRE(empty.by_extension.empty());
    SPACELENS_REQUIRE_EQ(empty.extension_other.extension_groups, 0ULL);
    for (const auto& g : empty.by_last_write_age) {
        SPACELENS_REQUIRE_EQ(g.file_count, 0ULL);
        SPACELENS_REQUIRE_EQ(g.logical_bytes, 0ULL);
    }
}

SPACELENS_TEST(Breakdown_missing_index_and_cancel)
{
    IsolatedDataRoot data;
    const auto missing = queryIndexedBreakdown(L"Q:\\no-such-index", {});
    SPACELENS_REQUIRE(!missing.ok);
    SPACELENS_REQUIRE_EQ(missing.error, std::string("index_not_found"));

    const std::wstring root = L"Q:\\";
    publishBreakdown(root, {fileRow(L"a.bin", 1, "bin", "Unknown", kNow)});
    std::stop_source src;
    src.request_stop();
    const auto cancelled = queryIndexedBreakdown(root, {}, src.get_token());
    SPACELENS_REQUIRE(!cancelled.ok);
    SPACELENS_REQUIRE_EQ(cancelled.error, std::string("cancelled"));
}

SPACELENS_TEST(Breakdown_json_contract)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    publishBreakdown(root, {fileRow(L"a.bin", 10, "bin", "Media", kNow)});
    const auto r = runBreakdown(root);
    const auto json = indexBreakdownToJson(r);
    SPACELENS_REQUIRE(json.find("\"schema_version\":1") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"command\":\"breakdown\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"source\":\"persistent_index\"") !=
                      std::string::npos);
    SPACELENS_REQUIRE(json.find("\"indexed_breakdown\"") == std::string::npos);
    SPACELENS_REQUIRE(json.find("safe_to_delete") == std::string::npos);
    SPACELENS_REQUIRE(json.find("\"fresh\":true") == std::string::npos);
    SPACELENS_REQUIRE(json.find("\"freshness\":") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"by_classification\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"by_extension\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"by_last_write_age\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"lt_30d\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"index_schema_version\":2") != std::string::npos);
}

SPACELENS_TEST(Breakdown_sum_overflow_is_estimated)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    constexpr ByteSize kHuge = 1ULL << 62;
    std::vector<BreakdownRow> rows;
    rows.push_back(fileRow(L"a.bin", kHuge, "bin", "Media", kNow));
    rows.push_back(fileRow(L"b.bin", kHuge, "bin", "Media", kNow));
    publishBreakdown(root, rows);

    const auto r = runBreakdown(root);
    if (!r.ok) {
        throw spacelens::test::Failure(std::string("overflow query failed: ") +
                                       r.error);
    }
    SPACELENS_REQUIRE(r.logical_bytes_estimated);
    SPACELENS_REQUIRE_EQ(r.total_file_count, 2ULL);
    const ByteSize exact = kHuge * 2ULL;  // 2^63, exact in IEEE-754
    SPACELENS_REQUIRE(r.total_logical_bytes == exact ||
                      r.total_logical_bytes ==
                          std::numeric_limits<ByteSize>::max());
    SPACELENS_REQUIRE(r.total_logical_bytes >
                      std::numeric_limits<ByteSize>::max() / 2ULL);
}

SPACELENS_TEST(Breakdown_sum_overflow_keeps_exact_other)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    constexpr ByteSize kHuge = 1ULL << 62;
    std::vector<BreakdownRow> rows;
    rows.push_back(fileRow(L"a.bin", kHuge, "bin", "Media", kNow));
    rows.push_back(fileRow(L"b.bin", kHuge, "bin", "Media", kNow));
    for (int i = 0; i < 21; ++i) {
        std::string ext = (i < 10) ? "e0" : "e";
        ext += std::to_string(i);
        rows.push_back(fileRow(L"f" + std::to_wstring(i), 1, ext, "Unknown",
                               kNow));
    }
    publishBreakdown(root, rows);

    const auto r = runBreakdown(root, {}, 20);
    if (!r.ok) {
        throw spacelens::test::Failure(std::string("overflow+other failed: ") +
                                       r.error);
    }
    SPACELENS_REQUIRE(r.logical_bytes_estimated);
    SPACELENS_REQUIRE_EQ(r.total_file_count, 23ULL);
    SPACELENS_REQUIRE_EQ(r.by_extension.size(), 20ULL);
    SPACELENS_REQUIRE_EQ(r.extension_other.extension_groups, 2ULL);
    SPACELENS_REQUIRE_EQ(r.extension_other.file_count, 2ULL);
    SPACELENS_REQUIRE_EQ(r.extension_other.logical_bytes, 2ULL);
    SPACELENS_REQUIRE(r.extension_other.logical_bytes !=
                      std::numeric_limits<ByteSize>::max());
    const auto* bin = findGroup(r.by_extension, "bin");
    SPACELENS_REQUIRE(bin != nullptr);
    SPACELENS_REQUIRE_EQ(bin->file_count, 2ULL);
    const auto* unknown = findGroup(r.by_classification, "Unknown");
    SPACELENS_REQUIRE(unknown != nullptr);
    SPACELENS_REQUIRE_EQ(unknown->file_count, 21ULL);
    SPACELENS_REQUIRE_EQ(unknown->logical_bytes, 21ULL);
}

SPACELENS_TEST(Breakdown_100k_sql_aggregation)
{
    IsolatedDataRoot data;
    const std::wstring root = L"Q:\\";
    constexpr int kCount = 100000;
    auto loc = locateIndex(root);
    {
        auto store = IndexStore::createStaging(loc);
        IndexRootInfo meta;
        meta.rootId = 1;
        meta.rootPath = loc.rootPath;
        meta.rootKey = loc.rootKey;
        meta.schemaVersion = kIndexSchemaVersion;
        meta.indexedAtTicks = kNow;
        meta.indexedAtIso = fileTimeTicksToIsoUtc(kNow);
        meta.fileCount = static_cast<std::uint64_t>(kCount);
        meta.dirCount = 2;
        meta.logicalBytes = static_cast<ByteSize>(kCount);
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
        auto bindDir = [&](const std::wstring& path, ByteSize rec) {
            insert.reset();
            insert.clearBindings();
            insert.bindInt64(1, id++);
            insert.bindInt64(2, 1);
            insert.bindNull(3);
            insert.bindInt64(4, 1);
            insert.bindText16(5, leafName(path));
            insert.bindText16(6, path);
            insert.bindInt64(7, 0);
            insert.bindInt64(8, static_cast<std::int64_t>(rec));
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
        };
        bindDir(loc.rootPath, static_cast<ByteSize>(kCount) * 100);
        bindDir(joinRoot(loc.rootPath, L"bulk"),
                static_cast<ByteSize>(kCount) * 100);
        for (int i = 0; i < kCount; ++i) {
            const std::wstring name = L"f-" + std::to_wstring(i) + L".bin";
            const std::wstring path = joinRoot(loc.rootPath, L"bulk\\" + name);
            const char* cls = (i % 3 == 0) ? "Media" : "Unknown";
            insert.reset();
            insert.clearBindings();
            insert.bindInt64(1, id++);
            insert.bindInt64(2, 1);
            insert.bindNull(3);
            insert.bindInt64(4, 0);
            insert.bindText16(5, name);
            insert.bindText16(6, path);
            insert.bindInt64(7, 1);
            insert.bindInt64(8, 1);
            insert.bindText(9, "bin");
            insert.bindInt64(10, static_cast<std::int64_t>(kNow));
            insert.bindInt64(11, 0);
            insert.bindInt64(12, 0);
            insert.bindInt64(13, 0);
            insert.bindText(14, cls);
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
        }
        txn.commit();
    }
    SPACELENS_REQUIRE(publishIndexDatabase(loc));

    const auto r = runBreakdown(root, {}, 20);
    requireReconcile(r);
    SPACELENS_REQUIRE_EQ(r.total_file_count, static_cast<std::uint64_t>(kCount));
    SPACELENS_REQUIRE_EQ(r.total_logical_bytes, static_cast<ByteSize>(kCount));
    SPACELENS_REQUIRE_EQ(r.by_extension.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(r.by_extension[0].key, std::string("bin"));
    SPACELENS_REQUIRE_EQ(r.extension_other.extension_groups, 0ULL);
    const auto* media = findGroup(r.by_classification, "Media");
    SPACELENS_REQUIRE(media);
    SPACELENS_REQUIRE_EQ(media->file_count, 33334ULL);
}
