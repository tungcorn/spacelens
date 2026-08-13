#include "TestRunner.hpp"

#include "core/ScanEngine.hpp"
#include "FakeFileEnumerator.hpp"

#include <stop_token>
#include <string>

using namespace spacelens;
using namespace spacelens::test;

SPACELENS_TEST(ScanEngine_nested_aggregation)
{
    FakeFileEnumerator fake;
    // Root
    //   a/ (dir)
    //     f1 100
    //   b/ (dir)
    //     f2 50
    //     c/ (dir)
    //       f3 25
    //   root.txt 10
    fake.setChildren(L"R", {
        makeDir(L"a"),
        makeDir(L"b"),
        makeFile(L"root.txt", 10),
    });
    fake.setChildren(L"R\\a", {makeFile(L"f1", 100)});
    fake.setChildren(L"R\\b", {makeFile(L"f2", 50), makeDir(L"c")});
    fake.setChildren(L"R\\b\\c", {makeFile(L"f3", 25)});

    ScanEngine engine(fake);
    ScanOptions options;
    options.topFileCount = 10;
    ScanResult result = engine.scan(L"R", options);

    SPACELENS_REQUIRE(result.state == ScanState::Completed);
    SPACELENS_REQUIRE(result.tree.dir(result.tree.root()).recursiveSize == 185);
    SPACELENS_REQUIRE(result.progress.filesSeen == 4);
    SPACELENS_REQUIRE(result.progress.directoriesSeen == 4); // R,a,b,c

    SPACELENS_REQUIRE(result.largestFiles.size() >= 1);
    SPACELENS_REQUIRE(result.largestFiles[0].size == 100);
}

SPACELENS_TEST(ScanEngine_empty_directory)
{
    FakeFileEnumerator fake;
    fake.setChildren(L"Empty", {});

    ScanEngine engine(fake);
    ScanResult result = engine.scan(L"Empty", {});

    SPACELENS_REQUIRE(result.state == ScanState::Completed);
    SPACELENS_REQUIRE(result.tree.dir(result.tree.root()).recursiveSize == 0);
    SPACELENS_REQUIRE(result.progress.filesSeen == 0);
    SPACELENS_REQUIRE(result.largestFiles.empty());
}

SPACELENS_TEST(ScanEngine_access_denied_nonfatal)
{
    FakeFileEnumerator fake;
    fake.setChildren(L"Root", {makeDir(L"secret"), makeFile(L"ok.txt", 5)});

    EnumerateResult denied;
    denied.status = EnumerateStatus::AccessDenied;
    fake.setListing(L"Root\\secret", std::move(denied));

    ScanEngine engine(fake);
    ScanResult result = engine.scan(L"Root", {});

    SPACELENS_REQUIRE(result.state == ScanState::Completed);
    SPACELENS_REQUIRE(result.tree.dir(result.tree.root()).recursiveSize == 5);
    SPACELENS_REQUIRE(result.progress.accessDenied == 1);
}

SPACELENS_TEST(ScanEngine_reparse_not_followed)
{
    FakeFileEnumerator fake;
    fake.setChildren(L"Root", {
        makeReparseDir(L"linked"),
        makeFile(L"real.txt", 3),
    });
    // If followed, this would be visited — it must not be.
    fake.setChildren(L"Root\\linked", {makeFile(L"hidden.bin", 9999)});

    ScanEngine engine(fake);
    ScanResult result = engine.scan(L"Root", {});

    SPACELENS_REQUIRE(result.state == ScanState::Completed);
    SPACELENS_REQUIRE(result.tree.dir(result.tree.root()).recursiveSize == 3);
    SPACELENS_REQUIRE(result.progress.reparsePointsSkipped == 1);
    SPACELENS_REQUIRE(result.progress.filesSeen == 1);
}

SPACELENS_TEST(ScanEngine_cancellation)
{
    FakeFileEnumerator fake;
    fake.setChildren(L"Root", {makeDir(L"a"), makeDir(L"b")});
    fake.setChildren(L"Root\\a", {makeFile(L"x", 1)});
    fake.setChildren(L"Root\\b", {makeFile(L"y", 2)});

    std::stop_source source;
    source.request_stop();

    ScanEngine engine(fake);
    ScanResult result = engine.scan(L"Root", {}, source.get_token());

    SPACELENS_REQUIRE(result.state == ScanState::Cancelled);
}

SPACELENS_TEST(ScanEngine_topk_order)
{
    FakeFileEnumerator fake;
    fake.setChildren(L"Root", {
        makeFile(L"small", 10),
        makeFile(L"huge", 1000),
        makeFile(L"mid", 100),
        makeFile(L"big", 500),
    });

    ScanEngine engine(fake);
    ScanOptions options;
    options.topFileCount = 2;
    ScanResult result = engine.scan(L"Root", options);

    SPACELENS_REQUIRE(result.largestFiles.size() == 2);
    SPACELENS_REQUIRE(result.largestFiles[0].size == 1000);
    SPACELENS_REQUIRE(result.largestFiles[1].size == 500);
}

SPACELENS_TEST(ScanEngine_deep_tree_does_not_overflow)
{
    FakeFileEnumerator fake;
    std::wstring path = L"R";
    fake.setChildren(path, {makeDir(L"d")});
    constexpr int kDepth = 80;
    for (int i = 0; i < kDepth; ++i) {
        path += L"\\d";
        if (i + 1 < kDepth) {
            fake.setChildren(path, {makeDir(L"d")});
        } else {
            fake.setChildren(path, {makeFile(L"leaf.bin", 7)});
        }
    }

    ScanEngine engine(fake);
    ScanResult result = engine.scan(L"R", {});
    SPACELENS_REQUIRE(result.state == ScanState::Completed);
    SPACELENS_REQUIRE(result.tree.dir(result.tree.root()).recursiveSize == 7);
    SPACELENS_REQUIRE(result.progress.filesSeen == 1);
    SPACELENS_REQUIRE(result.progress.directoriesSeen == static_cast<std::uint64_t>(kDepth + 1));
}
