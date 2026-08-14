#pragma once

#include "core/Duplicates.hpp"
#include "core/StorageIntelligence.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/index/IndexRefresh.hpp"

#include <cstddef>
#include <stop_token>
#include <string>
#include <string_view>

namespace spacelens {

inline constexpr std::size_t kMaxOverviewLimit = 100;
inline constexpr std::size_t kMaxOpportunityLimit = 100;
inline constexpr std::size_t kMaxQueryLimit = 200;

enum class AnalysisError {
    None,
    InaccessibleRoot,
    IndexNotFound,
    ScanFailed,
    Cancelled,
    InvalidArgument
};

[[nodiscard]] const char* toString(AnalysisError error) noexcept;

[[nodiscard]] bool pathExists(const std::wstring& path);
[[nodiscard]] bool pathIsDirectory(const std::wstring& path);
[[nodiscard]] FileTimeTicks currentFileTime();

struct OverviewRequest {
    std::wstring root;
    bool fromIndex = false;
    std::size_t limit = kDefaultOverviewLimit;
};

struct OverviewAnalysis {
    StorageOverviewReport report;
    AnalysisError error = AnalysisError::None;
};

[[nodiscard]] OverviewAnalysis analyzeOverview(const OverviewRequest& request,
                                               std::stop_token stop = {});

struct OpportunityRequest {
    std::wstring root;
    bool fromIndex = false;
    OpportunityQuery query{};
};

struct OpportunityAnalysis {
    OpportunityReport report;
    AnalysisError error = AnalysisError::None;
};

[[nodiscard]] OpportunityAnalysis analyzeOpportunities(
    const OpportunityRequest& request, std::stop_token stop = {});

struct DuplicateRequest {
    std::wstring root;
    ByteSize minSize = kDefaultDuplicateMinSize;
};

[[nodiscard]] DuplicateDetectionResult analyzeDuplicates(
    const DuplicateRequest& request, std::stop_token stop = {});

struct IndexStatusDocument {
    IndexQueryResult status;
    IndexRefreshResult probe;
};

[[nodiscard]] IndexStatusDocument analyzeIndexStatus(const std::wstring& root);

[[nodiscard]] std::string indexQueryToJson(const IndexQueryResult& result);
[[nodiscard]] std::string indexStatusToJson(const IndexQueryResult& status,
                                            const IndexRefreshResult& probe);
[[nodiscard]] std::string cliCapabilitiesJson();
[[nodiscard]] std::string mcpCapabilitiesJson();

[[nodiscard]] bool parseEvidenceSource(std::string_view text, bool& fromIndex,
                                       std::string& error);

}  // namespace spacelens
