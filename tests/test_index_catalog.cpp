#include "TestRunner.hpp"

#include "core/index/IndexBuilder.hpp"
#include "core/index/IndexCatalog.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexQuery.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace spacelens;

namespace {

ScanResult makeSyntheticScan(const std::wstring& root)
{
    ScanResult result;
    result.state = ScanState::Completed;
    const DirIndex r = result.tree.createRoot(root);
    const DirIndex build = result.tree.addDirectory(r, L"build");
    result.tree.addFile(build, L"CMakeCache.txt", 7, 100);
    result.tree.addFile(r, L"large.gguf", 4096, 100);
    result.tree.recomputeAggregates(1'000'000'000'000ULL);
    result.progress.filesSeen = result.tree.fileCount();
    result.progress.directoriesSeen = result.tree.directoryCount();
    result.progress.bytesSeen = result.tree.dir(r).recursiveSize;
    return result;
}

}  // namespace

SPACELENS_TEST(IndexCatalog_map_freshness_matrix)
{
    using S = IncrementalRefreshState;
    using O = IndexRefreshOutcome;

    SPACELENS_REQUIRE(mapIndexFreshness(false, 0, S::Unknown, O::IndexNotFound) ==
                      IndexFreshness::Missing);
    SPACELENS_REQUIRE(mapIndexFreshness(true, 0, S::Unknown, O::Failed) ==
                      IndexFreshness::Error);
    SPACELENS_REQUIRE(mapIndexFreshness(true, 1000, S::Supported, O::AlreadyCurrent) ==
                      IndexFreshness::RefreshAvailable);
    // AccessDenied stays IncrementalUnavailable even when outcome is the generic
    // full_rebuild_required used by refresh/probe reject paths.
    SPACELENS_REQUIRE(
        mapIndexFreshness(true, 1000, S::AccessDenied, O::FullRebuildRequired) ==
        IndexFreshness::IncrementalUnavailable);
    SPACELENS_REQUIRE(
        mapIndexFreshness(true, 1000, S::JournalChanged, O::FullRebuildRequired) ==
        IndexFreshness::FullRebuildRequired);
    SPACELENS_REQUIRE(
        mapIndexFreshness(true, 1000, S::HistoryLost, O::FullRebuildRequired) ==
        IndexFreshness::FullRebuildRequired);
    SPACELENS_REQUIRE(
        mapIndexFreshness(true, 1000, S::AccessDenied, O::AlreadyCurrent) ==
        IndexFreshness::IncrementalUnavailable);
    SPACELENS_REQUIRE(
        mapIndexFreshness(true, 1000, S::JournalNotActive, O::AlreadyCurrent) ==
        IndexFreshness::IncrementalUnavailable);
    SPACELENS_REQUIRE(mapIndexFreshness(true, 1000, S::Unknown, O::AlreadyCurrent) ==
                      IndexFreshness::Fresh);
    SPACELENS_REQUIRE(
        mapIndexFreshness(true, kFreshAgeMs + 1, S::Unknown, O::AlreadyCurrent) ==
        IndexFreshness::AgedSnapshot);
}

SPACELENS_TEST(IndexCatalog_makeBrowserQuerySpec)
{
    auto spec = makeBrowserQuerySpec(true, false, 1024, "gguf", "DownloadedAiModel",
                                     "Strong", 40);
    SPACELENS_REQUIRE(spec.includeFiles);
    SPACELENS_REQUIRE(!spec.includeDirectories);
    SPACELENS_REQUIRE(spec.minSize.has_value());
    SPACELENS_REQUIRE_EQ(*spec.minSize, 1024ULL);
    SPACELENS_REQUIRE(spec.extension == "gguf");
    SPACELENS_REQUIRE(spec.classification == "DownloadedAiModel");
    SPACELENS_REQUIRE(spec.candidateStrength == "Strong");
    SPACELENS_REQUIRE_EQ(spec.limit, 40ULL);

    auto defaults = makeBrowserQuerySpec(true, true, std::nullopt, "", "", "", 0);
    SPACELENS_REQUIRE_EQ(defaults.limit, 20ULL);
}

SPACELENS_TEST(IndexCatalog_summarize_existing_index)
{
    const std::wstring root = L"C:\\SpaceLensIndexUnitTestRoot\\catalog1";
    auto scan = makeSyntheticScan(root);
    auto built = buildIndexFromScan(scan, root, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);

    auto summary = summarizeIndexedRoot(root);
    SPACELENS_REQUIRE(summary.exists);
    SPACELENS_REQUIRE(summary.fileCount >= 1);
    SPACELENS_REQUIRE(summary.freshness != IndexFreshness::Missing);
    SPACELENS_REQUIRE(!summary.freshnessLabel.empty());
    // Without elevation, USN is typically AccessDenied → IncrementalUnavailable.
    SPACELENS_REQUIRE(summary.freshness == IndexFreshness::RefreshAvailable ||
                      summary.freshness == IndexFreshness::IncrementalUnavailable ||
                      summary.freshness == IndexFreshness::Fresh ||
                      summary.freshness == IndexFreshness::AgedSnapshot ||
                      summary.freshness == IndexFreshness::FullRebuildRequired);
}

SPACELENS_TEST(IndexCatalog_toString_stable)
{
    SPACELENS_REQUIRE(std::string(toString(IndexFreshness::RefreshAvailable)) ==
                      "refresh_available");
    SPACELENS_REQUIRE(std::string(toString(IndexFreshness::FullRebuildRequired)) ==
                      "full_rebuild_required");
}
