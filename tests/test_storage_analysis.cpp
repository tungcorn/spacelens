#include "TestRunner.hpp"

#include "core/DirectoryTree.hpp"
#include "core/FileTime.hpp"
#include "core/ScanTypes.hpp"
#include "core/StorageAnalysis.hpp"
#include "core/index/IndexBuilder.hpp"

#include <chrono>
#include <filesystem>
#include <string>

using namespace spacelens;

SPACELENS_TEST(StorageAnalysis_parse_evidence_source)
{
    bool fromIndex = true;
    std::string error;
    SPACELENS_REQUIRE(parseEvidenceSource("live_scan", fromIndex, error));
    SPACELENS_REQUIRE(!fromIndex);
    SPACELENS_REQUIRE(parseEvidenceSource("persistent_index", fromIndex, error));
    SPACELENS_REQUIRE(fromIndex);
    SPACELENS_REQUIRE(!parseEvidenceSource("index_refresh", fromIndex, error));
}

SPACELENS_TEST(StorageAnalysis_capabilities_flags)
{
    const auto cli = cliCapabilitiesJson();
    SPACELENS_REQUIRE(cli.find("\"filesystem_mutation\":false") != std::string::npos);
    SPACELENS_REQUIRE(cli.find("\"read_only\":true") != std::string::npos);
    SPACELENS_REQUIRE(cli.find("delete") == std::string::npos);

    const auto mcp = mcpCapabilitiesJson();
    SPACELENS_REQUIRE(mcp.find("\"filesystem_mutation\":false") != std::string::npos);
    SPACELENS_REQUIRE(mcp.find("\"read_only\":true") != std::string::npos);
    SPACELENS_REQUIRE(mcp.find("\"interface\":\"mcp\"") != std::string::npos);
    SPACELENS_REQUIRE(mcp.find("storage_overview") != std::string::npos);
    SPACELENS_REQUIRE(mcp.find("index_refresh") == std::string::npos ||
                      mcp.find("\"index_refresh\":false") != std::string::npos);
    SPACELENS_REQUIRE(mcp.find("storage_index_refresh") == std::string::npos);
}

SPACELENS_TEST(StorageAnalysis_overview_inaccessible)
{
    OverviewRequest request;
    request.root = L"C:\\SpaceLensDefinitelyMissingRoot\\nope";
    const auto analysis = analyzeOverview(request);
    SPACELENS_REQUIRE(analysis.error == AnalysisError::InaccessibleRoot);
    SPACELENS_REQUIRE(!analysis.report.ok);
    SPACELENS_REQUIRE(analysis.report.toJson().find("inaccessible_root") !=
                      std::string::npos);
}

SPACELENS_TEST(StorageAnalysis_index_status_missing)
{
    const auto doc =
        analyzeIndexStatus(L"C:\\SpaceLensDefinitelyNotIndexed\\nope");
    SPACELENS_REQUIRE(!doc.status.ok);
    const auto json = indexStatusToJson(doc.status, doc.probe);
    SPACELENS_REQUIRE(json.find("index_not_found") != std::string::npos);
    SPACELENS_REQUIRE(json.find("index_refresh") == std::string::npos ||
                      json.find("incremental_refresh") != std::string::npos);
}

SPACELENS_TEST(StorageAnalysis_indexed_classification_survives_fetch_cap)
{
    namespace fs = std::filesystem;
    const auto base =
        fs::temp_directory_path() / "spacelens_v2_idx_class" /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(base);
    const std::wstring root = base.wstring();

    constexpr FileTimeTicks kNow = 133800000000000000ULL;
    const FileTimeTicks recent = kNow - daysToTicks(10);

    ScanResult scan;
    scan.state = ScanState::Completed;
    const DirIndex r = scan.tree.createRoot(root);
    const int decoys =
        static_cast<int>(kIndexedOpportunityFetchLimit) + 1;
    for (int i = 0; i < decoys; ++i) {
        const DirIndex build =
            scan.tree.addDirectory(r, L"cmake-build-" + std::to_wstring(i));
        scan.tree.addFile(build, L"out.bin", 3ULL << 20, recent);
    }
    const DirIndex cache = scan.tree.addDirectory(r, L".cache");
    scan.tree.addFile(cache, L"tmp.dat", 3ULL << 19, recent);
    scan.tree.recomputeAggregates(kNow);
    scan.progress.filesSeen = scan.tree.fileCount();
    scan.progress.directoriesSeen = scan.tree.directoryCount();
    scan.progress.bytesSeen = scan.tree.dir(r).recursiveSize;

    const auto built = buildIndexFromScan(scan, root, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);

    OpportunityRequest request;
    request.root = root;
    request.fromIndex = true;
    request.query.minSize = 1;
    request.query.olderThanDays = 90;
    request.query.nowTicks = kNow;
    request.query.limit = 20;
    request.query.categoryOnly = StorageCategory::TemporaryData;

    const auto filtered = analyzeOpportunities(request);
    SPACELENS_REQUIRE(filtered.error == AnalysisError::None);
    SPACELENS_REQUIRE(filtered.report.ok);
    bool sawCache = false;
    for (const auto& item : filtered.report.opportunities) {
        SPACELENS_REQUIRE(item.classification == "TemporaryData");
        if (item.path.find(L".cache") != std::wstring::npos) {
            sawCache = true;
        }
    }
    SPACELENS_REQUIRE(sawCache);

    request.query.categoryOnly.reset();
    request.query.matchNone = true;
    const auto none = analyzeOpportunities(request);
    SPACELENS_REQUIRE(none.report.opportunities.empty());
}
