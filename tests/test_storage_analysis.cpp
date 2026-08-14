#include "TestRunner.hpp"

#include "core/StorageAnalysis.hpp"

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
