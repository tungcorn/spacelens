#include "TestRunner.hpp"

#include "core/PhysicalStorage.hpp"
#include "core/index/IndexStore.hpp"
#include "core/index/PhysicalAccounting.hpp"
#include "core/index/Sqlite.hpp"

#include <chrono>
#include <filesystem>
#include <limits>
#include <vector>

using namespace spacelens;
namespace fs = std::filesystem;

SPACELENS_TEST(Physical_coverage_complete_when_all_links_seen)
{
    SPACELENS_REQUIRE(classifyHardLinkCoverage(2, 2, 2, true) ==
                      HardLinkCoverage::Complete);
    SPACELENS_REQUIRE(classifyHardLinkCoverage(1, 1, 1, true) ==
                      HardLinkCoverage::Complete);
}

SPACELENS_TEST(Physical_coverage_incomplete_when_index_or_candidate_short)
{
    SPACELENS_REQUIRE(classifyHardLinkCoverage(3, 2, 3, true) ==
                      HardLinkCoverage::Incomplete);
    SPACELENS_REQUIRE(classifyHardLinkCoverage(3, 3, 1, true) ==
                      HardLinkCoverage::Incomplete);
}

SPACELENS_TEST(Physical_coverage_unknown_without_identity_or_fs_links)
{
    SPACELENS_REQUIRE(classifyHardLinkCoverage(2, 2, 2, false) ==
                      HardLinkCoverage::Unknown);
    SPACELENS_REQUIRE(classifyHardLinkCoverage(0, 1, 1, true) ==
                      HardLinkCoverage::Unknown);
}

SPACELENS_TEST(Physical_summarize_never_uses_logical_as_allocated)
{
    IdentityAllocation a;
    a.identity.fileId = 11;
    a.identity.volumeSerial = 7;
    a.allocatedBytes.reset();
    a.allocationKnown = false;
    a.filesystemLinks = 1;
    a.observedInIndex = 1;
    a.observedInCandidate = 1;

    const auto sum = summarizeIdentities({a});
    SPACELENS_REQUIRE(!sum.uniqueAllocatedBytes.has_value());
    SPACELENS_REQUIRE(!sum.exactReclaimBytes.has_value());
    SPACELENS_REQUIRE(!sum.allAllocationKnown);
    SPACELENS_REQUIRE(sum.coverage == HardLinkCoverage::Unknown ||
                      sum.unknownIdentityCount == 1);
}

SPACELENS_TEST(Physical_incomplete_hardlink_never_inflates_exact)
{
    IdentityAllocation complete;
    complete.identity.fileId = 1;
    complete.identity.volumeSerial = 9;
    complete.allocatedBytes = 1000;
    complete.allocationKnown = true;
    complete.filesystemLinks = 1;
    complete.observedInIndex = 1;
    complete.observedInCandidate = 1;

    IdentityAllocation incomplete;
    incomplete.identity.fileId = 2;
    incomplete.identity.volumeSerial = 9;
    incomplete.allocatedBytes = 50000;
    incomplete.allocationKnown = true;
    incomplete.filesystemLinks = 3;
    incomplete.observedInIndex = 1;
    incomplete.observedInCandidate = 1;

    const auto sum = summarizeIdentities({complete, incomplete});
    SPACELENS_REQUIRE(sum.uniqueAllocatedBytes.has_value());
    SPACELENS_REQUIRE_EQ(*sum.uniqueAllocatedBytes, 51000ULL);
    SPACELENS_REQUIRE(sum.exactReclaimBytes.has_value());
    SPACELENS_REQUIRE_EQ(*sum.exactReclaimBytes, 1000ULL);
    SPACELENS_REQUIRE(sum.coverage == HardLinkCoverage::Incomplete);
    SPACELENS_REQUIRE_EQ(sum.incompleteIdentityCount, 1ULL);
}

SPACELENS_TEST(Physical_complete_identities_are_unique)
{
    IdentityAllocation a;
    a.identity.fileId = 42;
    a.identity.volumeSerial = 1;
    a.allocatedBytes = 4096;
    a.allocationKnown = true;
    a.filesystemLinks = 2;
    a.observedInIndex = 2;
    a.observedInCandidate = 2;

    const auto sum = summarizeIdentities({a});
    SPACELENS_REQUIRE(sum.exactReclaimBytes.has_value());
    SPACELENS_REQUIRE_EQ(*sum.exactReclaimBytes, 4096ULL);
    SPACELENS_REQUIRE(sum.coverage == HardLinkCoverage::Complete);
}

SPACELENS_TEST(Physical_add_saturating_overflow)
{
    ByteSize total = std::numeric_limits<ByteSize>::max() - 5;
    SPACELENS_REQUIRE(addSaturating(total, 10));
    SPACELENS_REQUIRE_EQ(total, std::numeric_limits<ByteSize>::max());
    ByteSize small = 3;
    SPACELENS_REQUIRE(!addSaturating(small, 4));
    SPACELENS_REQUIRE_EQ(small, 7ULL);
}

SPACELENS_TEST(Physical_path_ancestry_is_component_safe)
{
    SPACELENS_REQUIRE(pathIsUnderNormalized(L"D:\\proj\\target\\x", L"D:\\proj\\target"));
    SPACELENS_REQUIRE(pathIsUnderNormalized(L"D:\\proj\\target", L"D:\\proj\\target"));
    SPACELENS_REQUIRE(!pathIsUnderNormalized(L"D:\\proj\\target2\\x", L"D:\\proj\\target"));
    SPACELENS_REQUIRE(!pathIsUnderNormalized(L"D:\\proj", L"D:\\proj\\target"));
}

SPACELENS_TEST(Physical_finalize_physical_accounting_updates_subdirectories)
{
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::temp_directory_path() / "spacelens_phys_test" /
                     std::to_string(stamp);
    fs::create_directories(dir);

    IndexLocation loc;
    loc.rootPath = L"C:\\TestRoot";
    loc.rootKey = L"testroot";
    loc.indexDir = dir.wstring();
    loc.dbPath = (dir / "index.db").wstring();
    loc.stagingDbPath = (dir / "index.db.building").wstring();

    auto store = IndexStore::createStaging(loc);
    IndexRootInfo meta;
    meta.rootId = 1;
    meta.rootPath = L"C:\\TestRoot";
    meta.rootKey = L"testroot";
    meta.schemaVersion = kIndexSchemaVersion;
    meta.indexedAtIso = "2026-01-01T00:00:00Z";
    meta.status = IndexStatus::Ready;
    store.writeRootMeta(meta);

    // Insert directory tree:
    // Root (id=1, parent=NULL)
    //   SubA (id=2, parent=1)
    //     SubB (id=3, parent=2)
    //       FileB (id=6, parent=3, allocated=1000)
    //     FileA (id=5, parent=2, allocated=2000)
    //   FileRoot (id=4, parent=1, allocated=3000)
    {
        SqliteStmt ins(store.db(),
                       "INSERT INTO entries(id, root_id, parent_id, kind, name, "
                       "path, size_bytes, recursive_size, volume_serial, file_id, "
                       "allocated_bytes, allocation_known, hard_link_count) "
                       "VALUES(?1, 1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 1, 1);");

        auto insertEntry = [&](std::int64_t id, std::optional<std::int64_t> parent,
                               int kind, const wchar_t* name, const wchar_t* path,
                               ByteSize size, std::uint64_t fileId,
                               ByteSize alloc) {
            ins.reset();
            ins.clearBindings();
            ins.bindInt64(1, id);
            if (parent.has_value()) {
                ins.bindInt64(2, *parent);
            } else {
                ins.bindNull(2);
            }
            ins.bindInt64(3, kind);
            ins.bindText16(4, name);
            ins.bindText16(5, path);
            ins.bindInt64(6, static_cast<std::int64_t>(size));
            ins.bindInt64(7, static_cast<std::int64_t>(size));
            ins.bindInt64(8, 100);  // volume_serial
            ins.bindInt64(9, static_cast<std::int64_t>(fileId));
            ins.bindInt64(10, static_cast<std::int64_t>(alloc));
            ins.stepDone();
        };

        // Dirs
        insertEntry(1, std::nullopt, 1, L"TestRoot", L"C:\\TestRoot", 6000, 101, 0);
        insertEntry(2, 1, 1, L"SubA", L"C:\\TestRoot\\SubA", 3000, 102, 0);
        insertEntry(3, 2, 1, L"SubB", L"C:\\TestRoot\\SubA\\SubB", 1000, 103, 0);

        // Files
        insertEntry(4, 1, 0, L"fileRoot.dat", L"C:\\TestRoot\\fileRoot.dat", 3000, 201, 3000);
        insertEntry(5, 2, 0, L"fileA.dat", L"C:\\TestRoot\\SubA\\fileA.dat", 2000, 202, 2000);
        insertEntry(6, 3, 0, L"fileB.dat", L"C:\\TestRoot\\SubA\\SubB\\fileB.dat", 1000, 203, 1000);
    }

    SPACELENS_REQUIRE(finalizePhysicalAccounting(store.db(), true));

    // Verify allocated_bytes on SubB (id=3) -> must be 1000
    // Verify allocated_bytes on SubA (id=2) -> must be 3000 (1000 + 2000)
    // Verify allocated_bytes on Root (id=1) -> must be 6000 (3000 + 2000 + 1000)
    {
        SqliteStmt query(store.db(), "SELECT id, allocated_bytes, hard_link_coverage FROM entries WHERE kind = 1 ORDER BY id ASC;");
        
        // Root (id=1)
        SPACELENS_REQUIRE(query.step());
        SPACELENS_REQUIRE_EQ(query.columnInt64(0), 1LL);
        SPACELENS_REQUIRE_EQ(query.columnInt64(1), 6000LL);
        SPACELENS_REQUIRE_EQ(query.columnText(2), "complete");

        // SubA (id=2)
        SPACELENS_REQUIRE(query.step());
        SPACELENS_REQUIRE_EQ(query.columnInt64(0), 2LL);
        SPACELENS_REQUIRE_EQ(query.columnInt64(1), 3000LL);
        SPACELENS_REQUIRE_EQ(query.columnText(2), "complete");

        // SubB (id=3)
        SPACELENS_REQUIRE(query.step());
        SPACELENS_REQUIRE_EQ(query.columnInt64(0), 3LL);
        SPACELENS_REQUIRE_EQ(query.columnInt64(1), 1000LL);
        SPACELENS_REQUIRE_EQ(query.columnText(2), "complete");
    }

    store.db().close();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

