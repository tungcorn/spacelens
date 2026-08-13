#pragma once

#include "core/CleanupReview.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spacelens {

enum class CleanupMetadataProbeOutcome {
    Present,
    Missing,
    AccessDenied,
    ProbeError
};

[[nodiscard]] const char* toString(CleanupMetadataProbeOutcome outcome) noexcept;

/// Metadata-only observation of a cleanup candidate path.
/// Directory recursive aggregates are never invented from this probe.
struct CleanupMetadataProbe {
    CleanupMetadataProbeOutcome outcome = CleanupMetadataProbeOutcome::ProbeError;
    std::uint32_t nativeError = 0;
    std::string detail;
    CleanupObjectEvidence objectEvidence{};
    bool isReparse = false;
};

/// Platform-neutral metadata reader. Implementations must not follow the final
/// reparse component and must not recurse or read file contents.
class ICleanupMetadataReader {
public:
    virtual ~ICleanupMetadataReader() = default;

    [[nodiscard]] virtual CleanupMetadataProbe read(const std::wstring& path) = 0;
};

struct CleanupRevalidation {
    CleanupMetadataProbe probe{};
    LocationSafety safety = LocationSafety::Unknown;
    CleanupCurrentEvidence current{};
    CleanupValidation validation{};
};

struct CleanupRevalidationPassResult {
    std::vector<CleanupValidationReplacement> updates;
    bool completed = false;
    std::size_t probedCount = 0;
};

/// Map a metadata probe into current evidence. AccessDenied/Error stay
/// unavailable so they cannot be reported as Missing or Unchanged.
[[nodiscard]] CleanupCurrentEvidence currentEvidenceFromProbe(
    const CleanupMetadataProbe& probe,
    LocationSafety safety) noexcept;

/// classifyLocation(candidate.path) + currentEvidenceFromProbe + validate.
[[nodiscard]] CleanupRevalidation revalidateCleanupCandidate(
    const CleanupCandidate& candidate,
    const CleanupMetadataProbe& probe);

[[nodiscard]] CleanupRevalidation revalidateCleanupCandidate(
    const CleanupCandidate& candidate,
    ICleanupMetadataReader& reader);

/// Probe the stored path and replaceValidation(). False if `id` is unknown.
/// Missing/denied/error never remove the review record.
[[nodiscard]] bool applyCleanupRevalidation(
    CleanupReview& review,
    std::uint64_t id,
    ICleanupMetadataReader& reader,
    FileTimeTicks checkedAt = 0);

/// Sequential metadata-only pass. One path at a time, no recursion, no
/// contents. If `cancelled` becomes true between items, `completed` is false
/// and callers must discard the partial `updates`.
[[nodiscard]] CleanupRevalidationPassResult probeCleanupReview(
    const CleanupReview& review,
    ICleanupMetadataReader& reader,
    FileTimeTicks checkedAt = 0,
    const std::function<bool()>& cancelled = {});

/// Best-effort add-time capture of direct object evidence and identity.
/// Missing/denied/error probes leave identity unavailable and never discard
/// the planning candidate. Directory recursive aggregates are not invented.
void attachLiveObjectEvidence(CleanupCandidate& candidate,
                              ICleanupMetadataReader& reader);

/// Preserve discovery aggregates, classify the path, stamp `addedAt` when
/// missing, then attach live object evidence. Does not discard the candidate.
void prepareCleanupCandidateForAdd(CleanupCandidate& candidate,
                                   ICleanupMetadataReader& reader,
                                   FileTimeTicks addedAt = 0);

}  // namespace spacelens
