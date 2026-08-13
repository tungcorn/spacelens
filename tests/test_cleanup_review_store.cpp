#include "TestRunner.hpp"

#include "core/CleanupReviewStore.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/Sqlite.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

using namespace spacelens;

namespace {

struct TempDir {
    std::filesystem::path path;

    TempDir()
    {
        namespace fs = std::filesystem;
        path = fs::temp_directory_path() / "spacelens_review_store_tests" /
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    [[nodiscard]] std::wstring dbPath() const
    {
        return (path / "state.db").wstring();
    }
};

CleanupIdentity strongId(std::uint8_t seed, std::uint64_t volume = 7)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = seed;
    bytes[15] = static_cast<std::uint8_t>(seed + 1);
    return makeFileId128Identity(volume, bytes);
}

CleanupCandidate fileCandidate(std::wstring path, ByteSize size, std::uint8_t seed)
{
    CleanupCandidate out;
    out.path = std::move(path);
    out.kind = ItemKind::File;
    out.sizeAtSelection = size;
    out.lastWriteTime = 11;
    out.attributes = 32;
    out.objectEvidence.available = true;
    out.objectEvidence.identity = strongId(seed);
    out.objectEvidence.kind = ItemKind::File;
    out.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
    out.objectEvidence.logicalSize = size;
    out.objectEvidence.lastWriteTime = 11;
    out.objectEvidence.lastAccessTime = 5;
    out.objectEvidence.attributes = 32;
    out.capturedSafety = LocationSafety::Ordinary;
    out.capturedReclaimability = Reclaimability::LikelyRegenerable;
    out.capturedCandidateStrength = CandidateStrength::Moderate;
    out.classification.category = StorageCategory::BuildArtifact;
    out.classification.confidence = Confidence::High;
    out.classification.ruleId = "test-rule";
    out.classification.reason = "fixture";
    out.source = "live_scan";
    out.reasonAdded = "manual";
    out.addedAt = 99;
    return out;
}

std::string metaValue(const std::wstring& dbPath, std::string_view key)
{
    SqliteDb db(dbPath, SqliteOpen::ReadOnly);
    SqliteStmt stmt(db, "SELECT value FROM meta WHERE key = ?1;");
    stmt.bindText(1, key);
    if (!stmt.step()) {
        return {};
    }
    return stmt.columnText(0);
}

bool tablePresent(const std::wstring& dbPath, const char* name)
{
    SqliteDb db(dbPath, SqliteOpen::ReadOnly);
    SqliteStmt stmt(
        db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1;");
    stmt.bindText(1, name);
    return stmt.step();
}

std::size_t userTableCount(const std::wstring& dbPath)
{
    SqliteDb db(dbPath, SqliteOpen::ReadOnly);
    SqliteStmt stmt(db,
                    "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                    "AND name NOT LIKE 'sqlite_%';");
    if (!stmt.step()) {
        return 0;
    }
    return static_cast<std::size_t>(stmt.columnInt64(0));
}

}  // namespace

SPACELENS_TEST(CleanupReviewStore_default_path_is_data_root_state_db)
{
    const auto root = spaceLensDataRoot();
    const auto state = spaceLensReviewStatePath();
    SPACELENS_REQUIRE(state.find(root) == 0);
    SPACELENS_REQUIRE(state.size() > 8);
    SPACELENS_REQUIRE(state.substr(state.size() - 8) == std::wstring(L"state.db"));
    SPACELENS_REQUIRE(state.find(L"indexes") == std::wstring::npos);
}

SPACELENS_TEST(CleanupReviewStore_schema_v1_created)
{
    TempDir dir;
    {
        CleanupReviewController controller;
        const auto opened = controller.open(dir.dbPath());
        SPACELENS_REQUIRE(opened.ok);
        SPACELENS_REQUIRE(controller.review().empty());
    }
    SPACELENS_REQUIRE_EQ(metaValue(dir.dbPath(), "review_schema_version"),
                         std::string("1"));
    SPACELENS_REQUIRE(tablePresent(dir.dbPath(), "review_items"));
    SPACELENS_REQUIRE(tablePresent(dir.dbPath(), "review_validation"));
    SPACELENS_REQUIRE(tablePresent(dir.dbPath(), "meta"));
}

SPACELENS_TEST(CleanupReviewStore_unicode_path_round_trip)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    auto item = fileCandidate(L"D:\\测试\\файл-文件.bin", 42, 1);
    const auto added = controller.addDetailed(std::move(item));
    SPACELENS_REQUIRE(added.ok);
    SPACELENS_REQUIRE(added.add.accepted());

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    SPACELENS_REQUIRE_EQ(reopened.review().size(), 1u);
    const auto loaded = reopened.review().items().front();
    SPACELENS_REQUIRE(loaded.path == std::wstring(L"D:\\测试\\файл-文件.bin"));
    SPACELENS_REQUIRE_EQ(loaded.sizeAtSelection, 42ULL);
}

SPACELENS_TEST(CleanupReviewStore_transactional_add_remove_clear_reopen)
{
    TempDir dir;
    std::uint64_t firstId = 0;
    std::uint64_t secondId = 0;
    {
        CleanupReviewController controller;
        SPACELENS_REQUIRE(controller.open(dir.dbPath()));
        auto first = fileCandidate(L"D:\\a.bin", 10, 1);
        first.sourceRoot = L"D:\\root";
        first.indexAgeMs = 12;
        first.indexIndexedAtIso = "2020-01-01T00:00:00Z";
        const auto added = controller.addDetailed(std::move(first));
        SPACELENS_REQUIRE(added.ok);
        firstId = added.add.id;
        SPACELENS_REQUIRE_EQ(firstId, 1ULL);

        CleanupCurrentEvidence current;
        current.available = true;
        current.exists = true;
        current.safety = LocationSafety::Ordinary;
        current.objectEvidence = objectEvidenceOf(*controller.review().findById(firstId));
        current.objectEvidence.logicalSize = 11;
        const auto validated =
            controller.replaceValidation(firstId, current, 1234);
        SPACELENS_REQUIRE(validated.ok);

        auto second = fileCandidate(L"D:\\b.bin", 20, 2);
        second.objectEvidence.identity = makeFileIndex64FallbackIdentity(9, 77);
        const auto addedSecond = controller.addDetailed(std::move(second));
        SPACELENS_REQUIRE(addedSecond.ok);
        secondId = addedSecond.add.id;
        SPACELENS_REQUIRE_EQ(secondId, 2ULL);
        SPACELENS_REQUIRE(controller.removeById(secondId).ok);
        SPACELENS_REQUIRE_EQ(controller.review().size(), 1u);
    }

    {
        CleanupReviewController controller;
        SPACELENS_REQUIRE(controller.open(dir.dbPath()));
        SPACELENS_REQUIRE_EQ(controller.review().size(), 1u);
        const auto loaded = *controller.review().findById(firstId);
        SPACELENS_REQUIRE_EQ(loaded.id, firstId);
        SPACELENS_REQUIRE_EQ(loaded.objectEvidence.identity.source,
                             CleanupIdentitySource::FileId128);
        SPACELENS_REQUIRE(identitiesEqual(loaded.objectEvidence.identity,
                                          strongId(1)));
        SPACELENS_REQUIRE(loaded.currentEvidence.available);
        SPACELENS_REQUIRE_EQ(loaded.currentEvidence.objectEvidence.logicalSize,
                             11ULL);
        SPACELENS_REQUIRE_EQ(loaded.validation.state,
                             CleanupValidationState::Changed);
        SPACELENS_REQUIRE(hasValidationReason(
            loaded.validation.reasons,
            CleanupValidationReason::LogicalSizeChanged));
        SPACELENS_REQUIRE_EQ(loaded.validationCheckedAt, 1234ULL);
        SPACELENS_REQUIRE_EQ(loaded.sourceRoot, std::wstring(L"D:\\root"));
        SPACELENS_REQUIRE_EQ(controller.review().nextId(), 3ULL);

        SPACELENS_REQUIRE(controller.clear().ok);
        SPACELENS_REQUIRE(controller.review().empty());
        auto again = fileCandidate(L"D:\\c.bin", 3, 3);
        const auto added = controller.addDetailed(std::move(again));
        SPACELENS_REQUIRE_EQ(added.add.id, 3ULL);
    }

    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    SPACELENS_REQUIRE_EQ(controller.review().size(), 1u);
    SPACELENS_REQUIRE_EQ(controller.review().items().front().id, 3ULL);
    const auto plan = controller.buildCleanupPlan();
    SPACELENS_REQUIRE_EQ(plan.summary.selectedCount, 1u);
    SPACELENS_REQUIRE(controller.copyReport().find("Cleanup Review") !=
                      std::string::npos);
}

SPACELENS_TEST(CleanupReviewStore_injected_write_failure_rolls_back)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    SPACELENS_REQUIRE(controller.add(fileCandidate(L"D:\\keep.bin", 8, 4)).ok);
    SPACELENS_REQUIRE_EQ(controller.review().size(), 1u);

    controller.failNextWrite();
    const auto failed = controller.add(fileCandidate(L"D:\\drop.bin", 9, 5));
    SPACELENS_REQUIRE(!failed.ok);
    SPACELENS_REQUIRE_EQ(failed.error, CleanupReviewError::WriteFailed);
    SPACELENS_REQUIRE_EQ(controller.review().size(), 1u);
    SPACELENS_REQUIRE(controller.review().containsPath(L"D:\\keep.bin"));
    SPACELENS_REQUIRE(!controller.review().containsPath(L"D:\\drop.bin"));

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    SPACELENS_REQUIRE_EQ(reopened.review().size(), 1u);
    SPACELENS_REQUIRE(reopened.review().containsPath(L"D:\\keep.bin"));
    SPACELENS_REQUIRE_EQ(reopened.review().nextId(), 2ULL);
}

SPACELENS_TEST(CleanupReviewStore_newer_schema_rejected_without_clobber)
{
    TempDir dir;
    {
        SqliteDb db(dir.dbPath(), SqliteOpen::ReadWrite | SqliteOpen::Create);
        db.exec(
            "CREATE TABLE meta (key TEXT PRIMARY KEY NOT NULL, value TEXT NOT "
            "NULL);");
        db.exec("CREATE TABLE review_items (id INTEGER PRIMARY KEY NOT NULL);");
        db.exec(
            "CREATE TABLE review_validation (review_item_id INTEGER PRIMARY KEY "
            "NOT NULL);");
        db.exec(
            "INSERT INTO meta(key, value) VALUES('review_schema_version', '2');");
        db.exec(
            "INSERT INTO meta(key, value) VALUES('keep_me', 'untouched');");
    }

    CleanupReviewController controller;
    const auto opened = controller.open(dir.dbPath());
    SPACELENS_REQUIRE(!opened.ok);
    SPACELENS_REQUIRE_EQ(opened.error, CleanupReviewError::SchemaUnsupported);
    SPACELENS_REQUIRE(!controller.isOpen());
    SPACELENS_REQUIRE(controller.review().empty());
    SPACELENS_REQUIRE_EQ(metaValue(dir.dbPath(), "review_schema_version"),
                         std::string("2"));
    SPACELENS_REQUIRE_EQ(metaValue(dir.dbPath(), "keep_me"),
                         std::string("untouched"));
    SPACELENS_REQUIRE(!tablePresent(dir.dbPath(), "roots"));
}

SPACELENS_TEST(CleanupReviewStore_malformed_schema_rejected_without_repair)
{
    TempDir dir;
    {
        SqliteDb db(dir.dbPath(), SqliteOpen::ReadWrite | SqliteOpen::Create);
        db.exec(
            "CREATE TABLE meta (key TEXT PRIMARY KEY NOT NULL, value TEXT NOT "
            "NULL);");
        db.exec(
            "INSERT INTO meta(key, value) VALUES('review_schema_version', '1');");
    }

    CleanupReviewController controller;
    const auto opened = controller.open(dir.dbPath());
    SPACELENS_REQUIRE(!opened.ok);
    SPACELENS_REQUIRE_EQ(opened.error, CleanupReviewError::SchemaMalformed);
    SPACELENS_REQUIRE(!tablePresent(dir.dbPath(), "review_items"));
    SPACELENS_REQUIRE(!tablePresent(dir.dbPath(), "review_validation"));
    SPACELENS_REQUIRE_EQ(userTableCount(dir.dbPath()), 1u);
}

SPACELENS_TEST(CleanupReviewStore_unknown_database_is_not_converted)
{
    TempDir dir;
    {
        SqliteDb db(dir.dbPath(), SqliteOpen::ReadWrite | SqliteOpen::Create);
        db.exec("CREATE TABLE roots (id INTEGER PRIMARY KEY NOT NULL);");
        db.exec(
            "CREATE TABLE meta (key TEXT PRIMARY KEY NOT NULL, value TEXT NOT "
            "NULL);");
        db.exec(
            "INSERT INTO meta(key, value) VALUES('index_schema_version', '2');");
    }

    CleanupReviewController controller;
    const auto opened = controller.open(dir.dbPath());
    SPACELENS_REQUIRE(!opened.ok);
    SPACELENS_REQUIRE_EQ(opened.error, CleanupReviewError::SchemaMalformed);
    SPACELENS_REQUIRE(!tablePresent(dir.dbPath(), "review_items"));
    SPACELENS_REQUIRE_EQ(metaValue(dir.dbPath(), "index_schema_version"),
                         std::string("2"));
}

SPACELENS_TEST(CleanupReviewStore_independent_of_source_index_db)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    SPACELENS_REQUIRE(controller.add(fileCandidate(L"D:\\keep.bin", 8, 6)).ok);

    const auto indexDir = dir.path / "indexes" / "deadbeef";
    std::filesystem::create_directories(indexDir);
    const auto indexDb = indexDir / "index.db";
    {
        std::ofstream(indexDb) << "dummy-index";
    }
    SPACELENS_REQUIRE_EQ(controller.review().size(), 1u);
    SPACELENS_REQUIRE_EQ(controller.review().items().front().id, 1ULL);

    std::error_code rebuildEc;
    std::filesystem::remove(indexDb, rebuildEc);
    {
        SqliteDb rebuilt(indexDb.wstring(),
                         SqliteOpen::ReadWrite | SqliteOpen::Create);
        rebuilt.exec("CREATE TABLE IF NOT EXISTS entries (id INTEGER);");
        rebuilt.exec("INSERT INTO entries(id) VALUES(1);");
    }
    SPACELENS_REQUIRE_EQ(controller.review().size(), 1u);
    SPACELENS_REQUIRE_EQ(controller.review().items().front().id, 1ULL);

    std::error_code ec;
    std::filesystem::remove(indexDb, ec);
    std::filesystem::remove_all(dir.path / "indexes", ec);

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    SPACELENS_REQUIRE_EQ(reopened.review().size(), 1u);
    SPACELENS_REQUIRE(reopened.review().containsPath(L"D:\\keep.bin"));
}

SPACELENS_TEST(CleanupReviewStore_same_path_identity_conflict_reloads)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    auto first = fileCandidate(L"D:\\same.bin", 10, 1);
    auto second = fileCandidate(L"D:\\same.bin", 20, 2);
    const auto a = controller.addDetailed(std::move(first));
    const auto b = controller.addDetailed(std::move(second));
    SPACELENS_REQUIRE(a.ok);
    SPACELENS_REQUIRE(b.ok);
    SPACELENS_REQUIRE(b.add.conflicted());
    SPACELENS_REQUIRE_EQ(controller.review().size(), 2u);

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    SPACELENS_REQUIRE_EQ(reopened.review().size(), 2u);
    const auto loadedA = *reopened.review().findById(a.add.id);
    const auto loadedB = *reopened.review().findById(b.add.id);
    SPACELENS_REQUIRE(identitiesEqual(loadedA.objectEvidence.identity, strongId(1)));
    SPACELENS_REQUIRE(identitiesEqual(loadedB.objectEvidence.identity, strongId(2)));
    SPACELENS_REQUIRE_EQ(loadedA.objectEvidence.identity.source,
                         CleanupIdentitySource::FileId128);
    SPACELENS_REQUIRE_EQ(loadedB.objectEvidence.identity.source,
                         CleanupIdentitySource::FileId128);
    SPACELENS_REQUIRE(!identitiesEqual(loadedA.objectEvidence.identity,
                                       loadedB.objectEvidence.identity));
}

SPACELENS_TEST(CleanupReviewStore_direct_directory_aggregate_stays_unavailable)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));

    CleanupCandidate direct;
    direct.path = L"D:\\direct";
    direct.kind = ItemKind::Directory;
    direct.sizeAtSelection = 100;
    direct.objectEvidence.available = true;
    direct.objectEvidence.identity = strongId(8);
    direct.objectEvidence.kind = ItemKind::Directory;
    direct.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
    direct.objectEvidence.logicalSize = 7;
    direct.historicalDirectoryAggregate.available = false;
    SPACELENS_REQUIRE(controller.add(std::move(direct)).ok);

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    const auto loaded = reopened.review().items().front();
    SPACELENS_REQUIRE(loaded.objectEvidence.available);
    SPACELENS_REQUIRE_EQ(loaded.objectEvidence.sizeScope,
                         CleanupEvidenceScope::Direct);
    SPACELENS_REQUIRE_EQ(loaded.objectEvidence.logicalSize, 7ULL);
    SPACELENS_REQUIRE(!loaded.historicalDirectoryAggregate.available);
    SPACELENS_REQUIRE(!loaded.historicalDirectoryAggregate.revalidated);
}

SPACELENS_TEST(CleanupReviewStore_refresh_evidence_keeps_directory_aggregate)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));

    CleanupCandidate directory;
    directory.path = L"D:\\project";
    directory.kind = ItemKind::Directory;
    directory.sizeAtSelection = 500;
    directory.objectEvidence.available = true;
    directory.objectEvidence.identity = strongId(9);
    directory.objectEvidence.kind = ItemKind::Directory;
    directory.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
    directory.objectEvidence.logicalSize = 0;
    directory.objectEvidence.lastWriteTime = 10;
    directory.historicalDirectoryAggregate.available = true;
    directory.historicalDirectoryAggregate.revalidated = false;
    directory.historicalDirectoryAggregate.recursiveLogicalSize = 500;
    directory.historicalDirectoryAggregate.newestDescendantWrite = 20;
    directory.capturedSafety = LocationSafety::Ordinary;
    const auto added = controller.addDetailed(std::move(directory));
    SPACELENS_REQUIRE(added.ok);

    CleanupCurrentEvidence current;
    current.available = true;
    current.exists = true;
    current.safety = LocationSafety::Sensitive;
    current.objectEvidence.available = true;
    current.objectEvidence.identity = strongId(9);
    current.objectEvidence.kind = ItemKind::Directory;
    current.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
    current.objectEvidence.logicalSize = 64;
    current.objectEvidence.lastWriteTime = 30;
    current.objectEvidence.attributes = 16;
    current.directoryAggregate.available = false;
    SPACELENS_REQUIRE(
        controller.replaceValidation(added.add.id, current, 50).ok);
    SPACELENS_REQUIRE(controller.refreshEvidence(added.add.id).ok);

    const auto refreshed = *controller.review().findById(added.add.id);
    SPACELENS_REQUIRE_EQ(refreshed.objectEvidence.logicalSize, 64ULL);
    SPACELENS_REQUIRE_EQ(refreshed.objectEvidence.lastWriteTime, 30ULL);
    SPACELENS_REQUIRE_EQ(refreshed.capturedSafety, LocationSafety::Sensitive);
    SPACELENS_REQUIRE(refreshed.historicalDirectoryAggregate.available);
    SPACELENS_REQUIRE_EQ(
        refreshed.historicalDirectoryAggregate.recursiveLogicalSize, 500ULL);
    SPACELENS_REQUIRE(
        !refreshed.historicalDirectoryAggregate.revalidated);
    SPACELENS_REQUIRE_EQ(refreshed.sizeAtSelection, 500ULL);

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    const auto loaded = *reopened.review().findById(added.add.id);
    SPACELENS_REQUIRE_EQ(loaded.objectEvidence.logicalSize, 64ULL);
    SPACELENS_REQUIRE(loaded.historicalDirectoryAggregate.available);
    SPACELENS_REQUIRE_EQ(
        loaded.historicalDirectoryAggregate.recursiveLogicalSize, 500ULL);
    SPACELENS_REQUIRE(!loaded.historicalDirectoryAggregate.revalidated);
}

SPACELENS_TEST(CleanupReviewStore_refresh_file_uses_current_direct_size)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    const auto added =
        controller.addDetailed(fileCandidate(L"D:\\file.bin", 10, 10));
    SPACELENS_REQUIRE(added.ok);

    CleanupCurrentEvidence current;
    current.available = true;
    current.exists = true;
    current.safety = LocationSafety::Ordinary;
    current.objectEvidence = objectEvidenceOf(*controller.review().findById(added.add.id));
    current.objectEvidence.logicalSize = 99;
    current.objectEvidence.lastWriteTime = 44;
    SPACELENS_REQUIRE(controller.replaceValidation(added.add.id, current).ok);
    SPACELENS_REQUIRE(controller.refreshEvidence(added.add.id).ok);
    const auto refreshed = *controller.review().findById(added.add.id);
    SPACELENS_REQUIRE_EQ(refreshed.sizeAtSelection, 99ULL);
    SPACELENS_REQUIRE_EQ(refreshed.objectEvidence.logicalSize, 99ULL);
    SPACELENS_REQUIRE_EQ(refreshed.lastWriteTime, 44ULL);
}

SPACELENS_TEST(CleanupReviewStore_refresh_without_current_evidence_is_noop)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    const auto added =
        controller.addDetailed(fileCandidate(L"D:\\file.bin", 10, 11));
    SPACELENS_REQUIRE(added.ok);
    const auto refreshed = controller.refreshEvidence(added.add.id);
    SPACELENS_REQUIRE(refreshed.ok);
    SPACELENS_REQUIRE(!refreshed.changed);
    SPACELENS_REQUIRE_EQ(controller.review().items().front().sizeAtSelection,
                         10ULL);
}

SPACELENS_TEST(CleanupReviewStore_identity_form_is_not_reinterpreted)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    auto fallback = fileCandidate(L"D:\\fallback.bin", 15, 12);
    fallback.objectEvidence.identity = makeFileIndex64FallbackIdentity(4, 88);
    SPACELENS_REQUIRE(controller.add(std::move(fallback)).ok);

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    const auto loaded = reopened.review().items().front();
    SPACELENS_REQUIRE_EQ(loaded.objectEvidence.identity.source,
                         CleanupIdentitySource::FileIndex64Fallback);
    SPACELENS_REQUIRE_EQ(loaded.objectEvidence.identity.volumeSerial, 4ULL);
    SPACELENS_REQUIRE_EQ(loaded.objectEvidence.identity.fileIndex64, 88ULL);
    SPACELENS_REQUIRE(!isStrongIdentity(loaded.objectEvidence.identity));
    SPACELENS_REQUIRE(isIdentityAvailable(loaded.objectEvidence.identity));
}

SPACELENS_TEST(CleanupReviewStore_failed_reopen_keeps_last_valid_memory)
{
    TempDir good;
    TempDir newer;
    {
        SqliteDb db(newer.dbPath(), SqliteOpen::ReadWrite | SqliteOpen::Create);
        db.exec(
            "CREATE TABLE meta (key TEXT PRIMARY KEY NOT NULL, value TEXT NOT "
            "NULL);");
        db.exec(
            "INSERT INTO meta(key, value) VALUES('review_schema_version', '9');");
    }

    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(good.dbPath()));
    SPACELENS_REQUIRE(controller.add(fileCandidate(L"D:\\keep.bin", 8, 13)).ok);
    const auto failed = controller.open(newer.dbPath());
    SPACELENS_REQUIRE(!failed.ok);
    SPACELENS_REQUIRE_EQ(failed.error, CleanupReviewError::SchemaUnsupported);
    SPACELENS_REQUIRE(controller.isOpen());
    SPACELENS_REQUIRE_EQ(controller.review().size(), 1u);
    SPACELENS_REQUIRE(controller.review().containsPath(L"D:\\keep.bin"));
    SPACELENS_REQUIRE_EQ(metaValue(newer.dbPath(), "review_schema_version"),
                         std::string("9"));
}

SPACELENS_TEST(CleanupReviewStore_access_denied_and_probe_error_reload)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    const auto deniedAdd =
        controller.addDetailed(fileCandidate(L"D:\\denied.bin", 4, 14));
    const auto errorAdd =
        controller.addDetailed(fileCandidate(L"D:\\error.bin", 5, 15));
    SPACELENS_REQUIRE(deniedAdd.ok);
    SPACELENS_REQUIRE(errorAdd.ok);

    CleanupCurrentEvidence denied;
    denied.available = false;
    denied.exists = true;
    denied.observation = CleanupObservation::AccessDenied;
    denied.safety = LocationSafety::Ordinary;
    SPACELENS_REQUIRE(
        controller.replaceValidation(deniedAdd.add.id, denied, 90).ok);

    CleanupCurrentEvidence error;
    error.available = false;
    error.exists = true;
    error.observation = CleanupObservation::ProbeError;
    error.safety = LocationSafety::Ordinary;
    SPACELENS_REQUIRE(
        controller.replaceValidation(errorAdd.add.id, error, 91).ok);

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    const auto deniedLoaded = *reopened.review().findById(deniedAdd.add.id);
    const auto errorLoaded = *reopened.review().findById(errorAdd.add.id);
    SPACELENS_REQUIRE_EQ(deniedLoaded.validation.state,
                         CleanupValidationState::AccessDenied);
    SPACELENS_REQUIRE_EQ(deniedLoaded.currentEvidence.observation,
                         CleanupObservation::AccessDenied);
    SPACELENS_REQUIRE_EQ(errorLoaded.validation.state,
                         CleanupValidationState::ProbeError);
    SPACELENS_REQUIRE_EQ(errorLoaded.currentEvidence.observation,
                         CleanupObservation::ProbeError);
    SPACELENS_REQUIRE(deniedLoaded.validationCheckedAt != 0);
    SPACELENS_REQUIRE(errorLoaded.validationCheckedAt != 0);
}

SPACELENS_TEST(CleanupReviewStore_validation_batch_write_failure_rolls_back)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    const auto first = controller.addDetailed(fileCandidate(L"D:\\a.bin", 4, 16));
    const auto second = controller.addDetailed(fileCandidate(L"D:\\b.bin", 5, 17));
    SPACELENS_REQUIRE(first.ok);
    SPACELENS_REQUIRE(second.ok);

    CleanupCurrentEvidence missing;
    missing.available = true;
    missing.exists = false;
    missing.observation = CleanupObservation::Missing;
    missing.safety = LocationSafety::Ordinary;

    std::vector<CleanupValidationReplacement> updates;
    updates.push_back({first.add.id, missing, 200});
    updates.push_back({second.add.id, missing, 200});

    controller.failNextWrite();
    const auto failed = controller.replaceValidationBatch(updates);
    SPACELENS_REQUIRE(!failed.ok);
    SPACELENS_REQUIRE_EQ(failed.error, CleanupReviewError::WriteFailed);
    SPACELENS_REQUIRE_EQ(controller.review().findById(first.add.id)->validation.state,
                         CleanupValidationState::NotValidated);
    SPACELENS_REQUIRE_EQ(
        controller.review().findById(second.add.id)->validation.state,
        CleanupValidationState::NotValidated);

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    SPACELENS_REQUIRE_EQ(reopened.review().findById(first.add.id)->validation.state,
                         CleanupValidationState::NotValidated);
    SPACELENS_REQUIRE_EQ(
        reopened.review().findById(second.add.id)->validation.state,
        CleanupValidationState::NotValidated);
}

SPACELENS_TEST(CleanupReviewStore_validation_batch_commits_only_complete_set)
{
    TempDir dir;
    CleanupReviewController controller;
    SPACELENS_REQUIRE(controller.open(dir.dbPath()));
    const auto first = controller.addDetailed(fileCandidate(L"D:\\a.bin", 4, 18));
    const auto second = controller.addDetailed(fileCandidate(L"D:\\b.bin", 5, 19));
    SPACELENS_REQUIRE(first.ok);
    SPACELENS_REQUIRE(second.ok);

    CleanupCurrentEvidence missing;
    missing.available = true;
    missing.exists = false;
    missing.observation = CleanupObservation::Missing;
    missing.safety = LocationSafety::Ordinary;

    std::vector<CleanupValidationReplacement> updates;
    updates.push_back({first.add.id, missing, 300});
    updates.push_back({second.add.id, missing, 300});
    SPACELENS_REQUIRE(controller.replaceValidationBatch(updates).ok);
    SPACELENS_REQUIRE_EQ(controller.review().findById(first.add.id)->validation.state,
                         CleanupValidationState::Missing);
    SPACELENS_REQUIRE_EQ(
        controller.review().findById(second.add.id)->validation.state,
        CleanupValidationState::Missing);

    CleanupReviewController reopened;
    SPACELENS_REQUIRE(reopened.open(dir.dbPath()));
    SPACELENS_REQUIRE_EQ(reopened.review().findById(first.add.id)->validation.state,
                         CleanupValidationState::Missing);
    SPACELENS_REQUIRE_EQ(
        reopened.review().findById(second.add.id)->validation.state,
        CleanupValidationState::Missing);
    SPACELENS_REQUIRE_EQ(
        reopened.review().findById(first.add.id)->validationCheckedAt, 300ULL);
}
