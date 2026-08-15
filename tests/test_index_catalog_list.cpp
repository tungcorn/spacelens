#include "TestRunner.hpp"

#include "core/FileTime.hpp"
#include "core/index/IndexCatalog.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/index/IndexSnapshot.hpp"
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
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
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
        m_dir = fs::temp_directory_path() / "spacelens_catalog_appdata" /
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

    [[nodiscard]] const std::filesystem::path& dir() const { return m_dir; }

private:
    std::filesystem::path m_dir;
    std::wstring m_previous;
    bool m_hadPrevious = false;
};

void publishMeta(const std::wstring& root, FileTimeTicks indexedAt = kNow,
                 std::uint64_t files = 1, std::uint64_t dirs = 2,
                 ByteSize bytes = 8ULL * 1024ULL * 1024ULL)
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
        meta.fileCount = files;
        meta.dirCount = dirs;
        meta.logicalBytes = bytes;
        meta.status = IndexStatus::Ready;
        store.writeRootMeta(meta);
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

const IndexCatalogEntry* findRoot(const IndexCatalogListing& listing,
                                  const std::wstring& root)
{
    const std::wstring want = normalizeIndexRoot(root);
    for (const auto& entry : listing.indexes) {
        if (compareIndexRootPath(entry.root, want) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

SPACELENS_TEST(CatalogList_empty)
{
    IsolatedDataRoot data;
    const auto listing = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE(listing.indexes.empty());
    SPACELENS_REQUIRE_EQ(listing.nowTicks, kNow);
    const auto json = indexCatalogToJson(listing);
    SPACELENS_REQUIRE(json.find("\"schema_version\":1") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"indexes\":[]") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"fresh\":") == std::string::npos);
    const auto human = formatIndexCatalogHuman(listing);
    SPACELENS_REQUIRE(human.find("No published indexes") != std::string::npos);
}

SPACELENS_TEST(CatalogList_one_root_known_full)
{
    IsolatedDataRoot data;
    const std::wstring root = L"C:\\SpaceLensCatalogNeverExists\\one";
    publishMeta(root, kNow - 1000ULL * kFileTimeTicksPerSecond);
    const auto listing = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), 1ULL);
    const auto& e = listing.indexes[0];
    SPACELENS_REQUIRE(compareIndexRootPath(e.root, root) == 0);
    SPACELENS_REQUIRE(e.status == IndexCatalogStatus::Ready);
    SPACELENS_REQUIRE_EQ(e.indexSchemaVersion, kIndexSchemaVersion);
    SPACELENS_REQUIRE(e.hasPublishedSnapshot);
    SPACELENS_REQUIRE(e.snapshot.ageState == SnapshotAgeState::Known);
    SPACELENS_REQUIRE(e.snapshot.publishKind == SnapshotPublishKind::Full);
    SPACELENS_REQUIRE(e.snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*e.snapshot.ageSeconds, 1000ULL);
    SPACELENS_REQUIRE(e.fileCount.has_value());
    SPACELENS_REQUIRE_EQ(*e.fileCount, 1ULL);
    SPACELENS_REQUIRE(e.logicalBytes.has_value());
    const auto json = indexCatalogToJson(listing);
    SPACELENS_REQUIRE(json.find("\"status\":\"ready\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"age_state\":\"known\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"publish_kind\":\"full\"") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"basis\":\"published_snapshot\"") !=
                      std::string::npos);
    SPACELENS_REQUIRE(json.find("\"fresh\":") == std::string::npos);
}

SPACELENS_TEST(CatalogList_incremental_publish_kind)
{
    IsolatedDataRoot data;
    const std::wstring root = L"C:\\SpaceLensCatalogNeverExists\\inc";
    const FileTimeTicks full = kNow - 5000ULL * kFileTimeTicksPerSecond;
    const FileTimeTicks refresh = kNow - 40ULL * kFileTimeTicksPerSecond;
    publishMeta(root, full);
    writeCheckpoint(root, refresh, "usn", full);
    const auto listing = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), 1ULL);
    const auto& e = listing.indexes[0];
    SPACELENS_REQUIRE(e.snapshot.publishKind == SnapshotPublishKind::Incremental);
    SPACELENS_REQUIRE(e.snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*e.snapshot.ageSeconds, 40ULL);
}

SPACELENS_TEST(CatalogList_unknown_and_clock_skew)
{
    IsolatedDataRoot data;
    publishMeta(L"C:\\SpaceLensCatalogNeverExists\\unknown", 0);
    publishMeta(L"C:\\SpaceLensCatalogNeverExists\\skew",
                kNow + 10ULL * kFileTimeTicksPerSecond);
    const auto listing = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), 2ULL);
    const auto* unknown =
        findRoot(listing, L"C:\\SpaceLensCatalogNeverExists\\unknown");
    const auto* skew = findRoot(listing, L"C:\\SpaceLensCatalogNeverExists\\skew");
    SPACELENS_REQUIRE(unknown != nullptr);
    SPACELENS_REQUIRE(skew != nullptr);
    SPACELENS_REQUIRE(unknown->snapshot.ageState == SnapshotAgeState::Unknown);
    SPACELENS_REQUIRE(!unknown->snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE(skew->snapshot.ageState == SnapshotAgeState::ClockSkew);
    SPACELENS_REQUIRE(!skew->snapshot.ageSeconds.has_value());
}

SPACELENS_TEST(CatalogList_deterministic_order_repeated)
{
    IsolatedDataRoot data;
    publishMeta(L"Z:\\SpaceLensCatalogOrder\\zzz");
    publishMeta(L"Z:\\SpaceLensCatalogOrder\\aaa");
    publishMeta(L"Z:\\SpaceLensCatalogOrder\\mmm");
    const auto a = listPublishedIndexes(kNow);
    const auto b = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE_EQ(a.indexes.size(), 3ULL);
    SPACELENS_REQUIRE_EQ(b.indexes.size(), 3ULL);
    SPACELENS_REQUIRE(a.indexes[0].root.find(L"aaa") != std::wstring::npos);
    SPACELENS_REQUIRE(a.indexes[1].root.find(L"mmm") != std::wstring::npos);
    SPACELENS_REQUIRE(a.indexes[2].root.find(L"zzz") != std::wstring::npos);
    for (std::size_t i = 0; i < a.indexes.size(); ++i) {
        SPACELENS_REQUIRE(a.indexes[i].root == b.indexes[i].root);
        SPACELENS_REQUIRE(a.indexes[i].rootKey == b.indexes[i].rootKey);
    }
}

SPACELENS_TEST(CatalogList_drive_root_and_unicode)
{
    IsolatedDataRoot data;
    const std::wstring drive = L"Z:\\";
    const std::wstring unicode = L"C:\\SpaceLensカタログ\\データ";
    publishMeta(drive, kNow - 5ULL * kFileTimeTicksPerSecond);
    publishMeta(unicode, kNow - 9ULL * kFileTimeTicksPerSecond);
    const auto listing = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), 2ULL);
    const auto* d = findRoot(listing, drive);
    const auto* u = findRoot(listing, unicode);
    SPACELENS_REQUIRE(d != nullptr);
    SPACELENS_REQUIRE(u != nullptr);
    SPACELENS_REQUIRE(d->status == IndexCatalogStatus::Ready);
    SPACELENS_REQUIRE(u->status == IndexCatalogStatus::Ready);
    SPACELENS_REQUIRE(u->root.find(L"カタログ") != std::wstring::npos);
    const auto json = indexCatalogToJson(listing);
    SPACELENS_REQUIRE(json.find("カタログ") != std::string::npos ||
                      json.find("\\u") != std::string::npos);
}

SPACELENS_TEST(CatalogList_list_status_parity_fixed_clock)
{
    IsolatedDataRoot data;
    const std::wstring root = L"C:\\SpaceLensCatalogNeverExists\\parity";
    const FileTimeTicks full = kNow - 8000ULL * kFileTimeTicksPerSecond;
    const FileTimeTicks refresh = kNow - 123ULL * kFileTimeTicksPerSecond;
    publishMeta(root, full);
    writeCheckpoint(root, refresh, "usn", full);
    const auto listing = listPublishedIndexes(kNow);
    const auto status = indexStatus(root, kNow);
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), 1ULL);
    SPACELENS_REQUIRE(status.ok);
    const auto& e = listing.indexes[0];
    SPACELENS_REQUIRE(compareIndexRootPath(e.root, status.location.rootPath) == 0);
    SPACELENS_REQUIRE_EQ(e.indexSchemaVersion, kIndexSchemaVersion);
    SPACELENS_REQUIRE(e.status == IndexCatalogStatus::Ready);
    SPACELENS_REQUIRE(status.root.status == IndexStatus::Ready);
    SPACELENS_REQUIRE(e.snapshot.ageState == status.snapshot.ageState);
    SPACELENS_REQUIRE(e.snapshot.publishKind == status.snapshot.publishKind);
    SPACELENS_REQUIRE_EQ(e.snapshot.publishedAtTicks,
                         status.snapshot.publishedAtTicks);
    SPACELENS_REQUIRE(e.snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE(status.snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*e.snapshot.ageSeconds, *status.snapshot.ageSeconds);
    SPACELENS_REQUIRE_EQ(*e.snapshot.ageSeconds, 123ULL);
}

SPACELENS_TEST(CatalogList_one_shared_now)
{
    IsolatedDataRoot data;
    const FileTimeTicks published = kNow - 77ULL * kFileTimeTicksPerSecond;
    publishMeta(L"C:\\SpaceLensCatalogNeverExists\\n1", published);
    publishMeta(L"C:\\SpaceLensCatalogNeverExists\\n2", published);
    const auto listing = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE_EQ(listing.nowTicks, kNow);
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), 2ULL);
    SPACELENS_REQUIRE(listing.indexes[0].snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE(listing.indexes[1].snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*listing.indexes[0].snapshot.ageSeconds, 77ULL);
    SPACELENS_REQUIRE_EQ(*listing.indexes[1].snapshot.ageSeconds, 77ULL);
}

SPACELENS_TEST(CatalogList_ignores_db_mtime)
{
    IsolatedDataRoot data;
    const std::wstring root = L"C:\\SpaceLensCatalogNeverExists\\mtime";
    publishMeta(root, kNow - 50ULL * kFileTimeTicksPerSecond);
    const auto loc = locateIndex(root);
    HANDLE file = ::CreateFileW(loc.dbPath.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SPACELENS_REQUIRE(file != INVALID_HANDLE_VALUE);
    FILETIME ancient{};
    ancient.dwLowDateTime = 1;
    ancient.dwHighDateTime = 1;
    const BOOL stamped = ::SetFileTime(file, &ancient, &ancient, &ancient);
    ::CloseHandle(file);
    SPACELENS_REQUIRE(stamped);
    const auto listing = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), 1ULL);
    SPACELENS_REQUIRE(listing.indexes[0].snapshot.ageSeconds.has_value());
    SPACELENS_REQUIRE_EQ(*listing.indexes[0].snapshot.ageSeconds, 50ULL);
}

SPACELENS_TEST(CatalogList_schema_incompatible_no_migrate)
{
    IsolatedDataRoot data;
    const std::wstring root = L"C:\\SpaceLensCatalogNeverExists\\oldschema";
    publishMeta(root);
    {
        SqliteDb db(locateIndex(root).dbPath, SqliteOpen::ReadWrite);
        db.exec("UPDATE meta SET value = '1' WHERE key = 'index_schema_version';");
    }
    const auto listing = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), 1ULL);
    SPACELENS_REQUIRE(listing.indexes[0].status == IndexCatalogStatus::Incompatible);
    SPACELENS_REQUIRE(listing.indexes[0].reason == "unsupported_schema");
    SPACELENS_REQUIRE_EQ(listing.indexes[0].indexSchemaVersion, 1);
    SPACELENS_REQUIRE(listing.indexes[0].hasPublishedSnapshot);
    auto inspect = IndexStore::openInspect(locateIndex(root));
    SPACELENS_REQUIRE_EQ(inspect.schemaVersion(), 1);
}

SPACELENS_TEST(CatalogList_broken_entry_isolated)
{
    IsolatedDataRoot data;
    publishMeta(L"C:\\SpaceLensCatalogNeverExists\\good");
    const std::wstring badRoot = L"C:\\SpaceLensCatalogNeverExists\\bad";
    auto badLoc = locateIndex(badRoot);
    SPACELENS_REQUIRE(ensureDirectory(badLoc.indexDir));
    {
        std::ofstream out(std::filesystem::path(badLoc.dbPath),
                          std::ios::binary | std::ios::trunc);
        out << "not a sqlite database";
    }
    const auto listing = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), 2ULL);
    bool sawGood = false;
    bool sawBad = false;
    for (const auto& e : listing.indexes) {
        if (e.status == IndexCatalogStatus::Ready &&
            e.root.find(L"good") != std::wstring::npos) {
            sawGood = true;
        }
        if (e.status != IndexCatalogStatus::Ready && !e.reason.empty()) {
            sawBad = true;
        }
    }
    SPACELENS_REQUIRE(sawGood);
    SPACELENS_REQUIRE(sawBad);
    const auto json = indexCatalogToJson(listing);
    SPACELENS_REQUIRE(json.find("\"reason\":") != std::string::npos);
}

SPACELENS_TEST(CatalogList_corrupt_missing_meta)
{
    IsolatedDataRoot data;
    const std::wstring root = L"C:\\SpaceLensCatalogNeverExists\\corrupt";
    publishMeta(root);
    {
        SqliteDb db(locateIndex(root).dbPath, SqliteOpen::ReadWrite);
        db.exec("DELETE FROM roots;");
    }
    const auto listing = listPublishedIndexes(kNow);
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), 1ULL);
    SPACELENS_REQUIRE(listing.indexes[0].status == IndexCatalogStatus::Corrupt);
    SPACELENS_REQUIRE(listing.indexes[0].reason == "index_corrupt");
    SPACELENS_REQUIRE(!listing.indexes[0].hasPublishedSnapshot);
}

SPACELENS_TEST(CatalogList_compare_index_root_path)
{
    SPACELENS_REQUIRE(compareIndexRootPath(L"D:\\Data", L"d:\\data") == 0);
    SPACELENS_REQUIRE(compareIndexRootPath(L"D:\\aaa", L"D:\\zzz") < 0);
    SPACELENS_REQUIRE(compareIndexRootPath(L"Z:\\", L"Z:\\foo") < 0);
}

SPACELENS_TEST(CatalogList_json_stdout_purity)
{
    IsolatedDataRoot data;
    publishMeta(L"C:\\SpaceLensCatalogNeverExists\\json");
    const auto json = indexCatalogToJson(listPublishedIndexes(kNow));
    SPACELENS_REQUIRE(!json.empty());
    SPACELENS_REQUIRE(json.front() == '{');
    SPACELENS_REQUIRE(json.find('\n') == json.size() - 1);
    SPACELENS_REQUIRE(json.find("Index snapshot") == std::string::npos);
    SPACELENS_REQUIRE(json.find("\"fresh\":") == std::string::npos);
    SPACELENS_REQUIRE(json.find("unique_review") == std::string::npos);
    SPACELENS_REQUIRE(json.find("usn_journal") == std::string::npos);
}

SPACELENS_TEST(CatalogList_scale_100_and_1000)
{
    IsolatedDataRoot data;
    constexpr int kCount = 1000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = kCount - 1; i >= 0; --i) {
        wchar_t buf[64]{};
        std::swprintf(buf, 64, L"C:\\SpaceLensCatalogScale\\%04d", i);
        publishMeta(buf, kNow - 10ULL * kFileTimeTicksPerSecond, 3, 4, 4096);
    }
    const auto built = std::chrono::steady_clock::now();
    const auto listing = listPublishedIndexes(kNow);
    const auto listed = std::chrono::steady_clock::now();
    SPACELENS_REQUIRE_EQ(listing.indexes.size(), static_cast<std::size_t>(kCount));
    SPACELENS_REQUIRE(listing.indexes.front().root.find(L"0000") !=
                      std::wstring::npos);
    SPACELENS_REQUIRE(listing.indexes.back().root.find(L"0999") !=
                      std::wstring::npos);
    for (const auto& e : listing.indexes) {
        SPACELENS_REQUIRE(e.status == IndexCatalogStatus::Ready);
        SPACELENS_REQUIRE(e.hasPublishedSnapshot);
        SPACELENS_REQUIRE(e.snapshot.ageSeconds.has_value());
        SPACELENS_REQUIRE_EQ(*e.snapshot.ageSeconds, 10ULL);
    }
    const auto json = indexCatalogToJson(listing);
    const auto json100 = indexCatalogToJson([&] {
        IndexCatalogListing slice;
        slice.nowTicks = kNow;
        slice.indexes.assign(listing.indexes.begin(),
                             listing.indexes.begin() + 100);
        return slice;
    }());
    const auto json10 = indexCatalogToJson([&] {
        IndexCatalogListing slice;
        slice.nowTicks = kNow;
        slice.indexes.assign(listing.indexes.begin(), listing.indexes.begin() + 10);
        return slice;
    }());
    SPACELENS_REQUIRE(json10.size() < 16ULL * 1024ULL);
    SPACELENS_REQUIRE(json100.size() < 128ULL * 1024ULL);
    SPACELENS_REQUIRE(json.size() < 1024ULL * 1024ULL);
    SPACELENS_REQUIRE(json.size() / static_cast<std::size_t>(kCount) < 2048ULL);
    const auto listMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            listed - built)
                            .count();
    (void)t0;
    SPACELENS_REQUIRE(listMs < 30000);
}
