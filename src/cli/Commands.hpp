#pragma once

#include "cli/Args.hpp"
#include "cli/ExitCodes.hpp"

#include <stop_token>

namespace spacelens::cli {

[[nodiscard]] ExitCode runScan(const ParsedArgs& args, std::stop_token stop);
[[nodiscard]] ExitCode runTop(const ParsedArgs& args, std::stop_token stop);
[[nodiscard]] ExitCode runFind(const ParsedArgs& args, std::stop_token stop);
[[nodiscard]] ExitCode runIndex(const ParsedArgs& args, std::stop_token stop);
[[nodiscard]] ExitCode runIndexStatus(const ParsedArgs& args);
[[nodiscard]] ExitCode runIndexList(const ParsedArgs& args);
[[nodiscard]] ExitCode runIndexRefresh(const ParsedArgs& args,
                                       std::stop_token stop);
[[nodiscard]] ExitCode runQuery(const ParsedArgs& args);
[[nodiscard]] ExitCode runDuplicates(const ParsedArgs& args, std::stop_token stop);
[[nodiscard]] ExitCode runOverview(const ParsedArgs& args, std::stop_token stop);
[[nodiscard]] ExitCode runOpportunities(const ParsedArgs& args,
                                        std::stop_token stop);
[[nodiscard]] ExitCode runBreakdown(const ParsedArgs& args,
                                    std::stop_token stop);
[[nodiscard]] ExitCode runCapabilities(const ParsedArgs& args);

}  // namespace spacelens::cli
