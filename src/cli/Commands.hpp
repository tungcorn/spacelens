#pragma once

#include "cli/Args.hpp"
#include "cli/ExitCodes.hpp"

#include <stop_token>

namespace spacelens::cli {

[[nodiscard]] ExitCode runScan(const ParsedArgs& args, std::stop_token stop);
[[nodiscard]] ExitCode runTop(const ParsedArgs& args, std::stop_token stop);

}  // namespace spacelens::cli
