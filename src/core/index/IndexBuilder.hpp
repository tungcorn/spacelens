#pragma once

#include "core/ScanTypes.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexStore.hpp"

#include <stop_token>
#include <string>

namespace spacelens {

enum class IndexBuildState {
    Completed,
    Cancelled,
    Failed
};

struct IndexBuildResult {
    IndexBuildState state = IndexBuildState::Failed;
    IndexLocation location{};
    IndexRootInfo root{};
    std::string error;
    double elapsedSeconds = 0.0;
};

/// Full rebuild of a root index from a completed ScanResult snapshot.
/// Writes only to staging; publishes on success. Cancel/fail leaves the
/// previous published index intact.
[[nodiscard]] IndexBuildResult buildIndexFromScan(
    const ScanResult& scan,
    const std::wstring& rootPath,
    std::stop_token stop = {});

/// Scan the filesystem then build an index (convenience for CLI).
[[nodiscard]] IndexBuildResult buildIndexForRoot(
    const std::wstring& rootPath,
    std::stop_token stop = {});

}  // namespace spacelens
