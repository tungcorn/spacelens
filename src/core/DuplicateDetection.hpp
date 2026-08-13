#pragma once

#include "core/CleanupRevalidation.hpp"
#include "core/Duplicates.hpp"

#include <functional>

namespace spacelens {

/// Sequential, cancellable, read-only duplicate verification.
/// Same-size and sample-fingerprint matches are never published as verified
/// groups. Cancellation keeps only groups that already finished verification
/// and marks the result cancelled/partial.
[[nodiscard]] DuplicateDetectionResult detectDuplicates(
    const DuplicateCandidateQueryResult& candidates,
    ICleanupMetadataReader& reader,
    IFileContentHasher& hasher,
    const DuplicateScanOptions& options = {},
    const std::function<bool()>& cancelled = {},
    const std::function<void(const DuplicateScanProgress&)>& onProgress = {});

}  // namespace spacelens
