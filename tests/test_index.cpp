#include "TestRunner.hpp"

#include "core/DirectoryTree.hpp"
#include "core/ScanTypes.hpp"
#include "core/index/IndexBuilder.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/index/IndexStore.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace spacelens;

namespace {

std::wstring testRoot()
{
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "spacelens_index_tests" /
                      std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(base / "build" / "CMakeFiles");
    fs::create_directories(base / "node_modules");
    {
        std::ofstream(base / "build" / "CMakeCache.txt") << "cmake\n";
        std::ofstream(base / "large.gguf") << std::string(4096, 'x');
        std::ofstream(base / "photo.jpg") << "img";
        std::ofstream(base / "node_modules" / "pkg.js") << "js";
        std::ofstream(base / L"unicodé-文件.txt") << "u";
    }
    return base.wstring();
}

ScanResult makeSyntheticScan(const std::wstring& root)
{
    ScanResult result;
    result.state = ScanState::Completed;
    const DirIndex r = result.tree.createRoot(root);
    const DirIndex build = result.tree.addDirectory(r, L"build");
    result.tree.addFile(build, L"CMakeCache.txt", 7, 100);
    result.tree.addDirectory(build, L"CMakeFiles");
    const DirIndex nm = result.tree.addDirectory(r, L"node_modules");
    result.tree.addFile(nm, L"pkg.js", 2, 100);
    result.tree.addFile(r, L"large.gguf", 4096, 100);
    result.tree.addFile(r, L"photo.jpg", 3, 100);
    result.tree.recomputeAggregates(1'000'000'000'000ULL);
    result.progress.filesSeen = result.tree.fileCount();
    result.progress.directoriesSeen = result.tree.directoryCount();
    result.progress.bytesSeen = result.tree.dir(r).recursiveSize;
    return result;
}

}  // namespace

SPACELENS_TEST(Index_build_reopen_query)
{
    const std::wstring root = L"C:\\SpaceLensIndexUnitTestRoot\\tree1";
    auto scan = makeSyntheticScan(root);
    auto built = buildIndexFromScan(scan, root, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);
    SPACELENS_REQUIRE(indexDatabaseExists(built.location));

    auto status = indexStatus(root);
    SPACELENS_REQUIRE(status.ok);
    SPACELENS_REQUIRE_EQ(status.root.fileCount, scan.progress.filesSeen);

    IndexQuerySpec files;
    files.includeFiles = true;
    files.includeDirectories = false;
    files.limit = 50;
    auto q = queryIndex(root, files);
    SPACELENS_REQUIRE(q.ok);
    SPACELENS_REQUIRE(q.returned_items >= 1);

    // Deterministic ordering: sizes descending.
    for (std::size_t i = 1; i < q.hits.size(); ++i) {
        SPACELENS_REQUIRE(q.hits[i - 1].size_bytes >= q.hits[i].size_bytes);
    }

    IndexQuerySpec ext;
    ext.includeFiles = true;
    ext.extension = "gguf";
    ext.limit = 10;
    auto q2 = queryIndex(root, ext);
    SPACELENS_REQUIRE(q2.ok);
    SPACELENS_REQUIRE_EQ(q2.returned_items, 1ULL);
    SPACELENS_REQUIRE(q2.hits.front().classification == "DownloadedAiModel");
}

SPACELENS_TEST(Index_missing_returns_not_found)
{
    const auto r = queryIndex(L"C:\\SpaceLensDefinitelyNotIndexed\\nope",
                              IndexQuerySpec{});
    SPACELENS_REQUIRE(!r.ok);
    SPACELENS_REQUIRE(r.error == "index_not_found");
}

SPACELENS_TEST(Index_rebuild_preserves_on_cancel_before_publish)
{
    const std::wstring root = L"C:\\SpaceLensIndexUnitTestRoot\\tree2";
    auto scan = makeSyntheticScan(root);
    auto first = buildIndexFromScan(scan, root, {});
    SPACELENS_REQUIRE(first.state == IndexBuildState::Completed);

    // Second build with already-stopped token during insert path: start stop
    // after a completed first index; a pre-stopped token cancels before publish
    // of a new staging build.
    std::stop_source src;
    src.request_stop();
    auto second = buildIndexFromScan(scan, root, src.get_token());
    // Cancel may happen during insert loops immediately.
    SPACELENS_REQUIRE(second.state == IndexBuildState::Cancelled ||
                      second.state == IndexBuildState::Completed);
    // Previous valid index must still exist.
    SPACELENS_REQUIRE(indexDatabaseExists(first.location));
    auto status = indexStatus(root);
    SPACELENS_REQUIRE(status.ok);
}

SPACELENS_TEST(Index_query_no_live_fallback_fields)
{
    const std::wstring root = L"C:\\SpaceLensIndexUnitTestRoot\\tree3";
    auto scan = makeSyntheticScan(root);
    auto built = buildIndexFromScan(scan, root, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);

    IndexQuerySpec dirs;
    dirs.includeFiles = false;
    dirs.includeDirectories = true;
    dirs.limit = 20;
    auto q = queryIndex(root, dirs);
    SPACELENS_REQUIRE(q.ok);
    SPACELENS_REQUIRE(q.age_ms >= 0);
    bool sawNodeModules = false;
    for (const auto& h : q.hits) {
        if (h.path.find(L"node_modules") != std::wstring::npos) {
            sawNodeModules = true;
            SPACELENS_REQUIRE(h.classification == "DependencyDirectory" ||
                              h.classification == "Unknown" ||
                              h.classification == "BuildArtifact");
        }
    }
    SPACELENS_REQUIRE(sawNodeModules);
}

SPACELENS_TEST(Index_query_min_size_and_count_parity)
{
    const std::wstring root = L"C:\\SpaceLensIndexUnitTestRoot\\tree4";
    auto scan = makeSyntheticScan(root);
    auto built = buildIndexFromScan(scan, root, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);

    // large.gguf is 4096 bytes; minSize 4000 should match it, 5000 should not.
    IndexQuerySpec minOk;
    minOk.includeFiles = true;
    minOk.includeDirectories = false;
    minOk.minSize = 4000;
    minOk.limit = 50;
    auto qOk = queryIndex(root, minOk);
    SPACELENS_REQUIRE(qOk.ok);
    SPACELENS_REQUIRE(qOk.matched_items >= 1);
    SPACELENS_REQUIRE(qOk.returned_items >= 1);
    SPACELENS_REQUIRE(qOk.hits.front().size_bytes >= 4000);

    IndexQuerySpec minNone;
    minNone.includeFiles = true;
    minNone.includeDirectories = false;
    minNone.minSize = 5000;
    minNone.limit = 50;
    auto qNone = queryIndex(root, minNone);
    SPACELENS_REQUIRE(qNone.ok);
    SPACELENS_REQUIRE_EQ(qNone.matched_items, 0ULL);
    SPACELENS_REQUIRE_EQ(qNone.returned_items, 0ULL);

    // Indexed file count matches the scan snapshot used to build.
    auto status = indexStatus(root);
    SPACELENS_REQUIRE(status.ok);
    SPACELENS_REQUIRE_EQ(status.root.fileCount, scan.progress.filesSeen);
    SPACELENS_REQUIRE_EQ(status.root.dirCount, scan.progress.directoriesSeen);
}
