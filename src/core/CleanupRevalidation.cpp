#include "core/CleanupRevalidation.hpp"

#include "core/SafetyPolicy.hpp"

#include <utility>

namespace spacelens {

const char* toString(CleanupMetadataProbeOutcome outcome) noexcept
{
    switch (outcome) {
    case CleanupMetadataProbeOutcome::Present:
        return "Present";
    case CleanupMetadataProbeOutcome::Missing:
        return "Missing";
    case CleanupMetadataProbeOutcome::AccessDenied:
        return "AccessDenied";
    case CleanupMetadataProbeOutcome::ProbeError:
        return "ProbeError";
    }
    return "ProbeError";
}

CleanupCurrentEvidence currentEvidenceFromProbe(const CleanupMetadataProbe& probe,
                                                LocationSafety safety) noexcept
{
    CleanupCurrentEvidence current;
    current.safety = safety;
    current.directoryAggregate.available = false;
    current.directoryAggregate.revalidated = false;

    switch (probe.outcome) {
    case CleanupMetadataProbeOutcome::Present:
        if (!probe.objectEvidence.available) {
            current.available = false;
            current.exists = true;
            current.observation = CleanupObservation::ProbeError;
            return current;
        }
        current.available = true;
        current.exists = true;
        current.observation = CleanupObservation::Present;
        current.objectEvidence = probe.objectEvidence;
        return current;
    case CleanupMetadataProbeOutcome::Missing:
        // Distinguishable from AccessDenied/ProbeError: the object is gone.
        current.available = true;
        current.exists = false;
        current.observation = CleanupObservation::Missing;
        return current;
    case CleanupMetadataProbeOutcome::AccessDenied:
        // Do not claim Missing or Unchanged when the probe could not observe.
        current.available = false;
        current.exists = true;
        current.observation = CleanupObservation::AccessDenied;
        return current;
    case CleanupMetadataProbeOutcome::ProbeError:
        current.available = false;
        current.exists = true;
        current.observation = CleanupObservation::ProbeError;
        return current;
    }
    current.available = false;
    current.exists = true;
    current.observation = CleanupObservation::ProbeError;
    return current;
}

CleanupRevalidation revalidateCleanupCandidate(const CleanupCandidate& candidate,
                                               const CleanupMetadataProbe& probe)
{
    CleanupRevalidation out;
    out.probe = probe;
    out.safety = classifyLocation(candidate.path);
    out.current = currentEvidenceFromProbe(probe, out.safety);
    out.validation = validateCleanupCandidate(candidate, out.current);
    return out;
}

CleanupRevalidation revalidateCleanupCandidate(const CleanupCandidate& candidate,
                                               ICleanupMetadataReader& reader)
{
    return revalidateCleanupCandidate(candidate, reader.read(candidate.path));
}

bool applyCleanupRevalidation(CleanupReview& review,
                              std::uint64_t id,
                              ICleanupMetadataReader& reader,
                              FileTimeTicks checkedAt)
{
    const auto item = review.findById(id);
    if (!item) {
        return false;
    }
    auto result = revalidateCleanupCandidate(*item, reader);
    return review.replaceValidation(id, std::move(result.current), checkedAt);
}

CleanupRevalidationPassResult probeCleanupReview(
    const CleanupReview& review,
    ICleanupMetadataReader& reader,
    FileTimeTicks checkedAt,
    const std::function<bool()>& cancelled)
{
    CleanupRevalidationPassResult out;
    out.updates.reserve(review.size());
    for (const auto& item : review.items()) {
        if (cancelled && cancelled()) {
            out.completed = false;
            out.updates.clear();
            return out;
        }
        auto result = revalidateCleanupCandidate(item, reader);
        CleanupValidationReplacement update;
        update.id = item.id;
        update.current = std::move(result.current);
        update.checkedAt = checkedAt;
        out.updates.push_back(std::move(update));
        ++out.probedCount;
    }
    out.completed = true;
    return out;
}

void attachLiveObjectEvidence(CleanupCandidate& candidate,
                              ICleanupMetadataReader& reader)
{
    const auto probe = reader.read(candidate.path);
    if (probe.outcome != CleanupMetadataProbeOutcome::Present ||
        !probe.objectEvidence.available) {
        return;
    }
    candidate.objectEvidence = probe.objectEvidence;
    candidate.attributes = probe.objectEvidence.attributes;
    if (candidate.kind == ItemKind::File) {
        candidate.lastWriteTime = probe.objectEvidence.lastWriteTime;
        if (candidate.sizeAtSelection == 0) {
            candidate.sizeAtSelection = probe.objectEvidence.logicalSize;
        }
        return;
    }
    // Directories keep discovery recursive size as historical aggregate.
    // Object evidence stays direct and must not overwrite that aggregate.
    if (candidate.objectEvidence.sizeScope == CleanupEvidenceScope::Recursive) {
        candidate.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
        candidate.objectEvidence.logicalSize = 0;
        candidate.objectEvidence.lastWriteTime = 0;
    }
}

void prepareCleanupCandidateForAdd(CleanupCandidate& candidate,
                                   ICleanupMetadataReader& reader,
                                   FileTimeTicks addedAt)
{
    if (candidate.kind != ItemKind::File &&
        !candidate.historicalDirectoryAggregate.available) {
        candidate.historicalDirectoryAggregate.available = true;
        candidate.historicalDirectoryAggregate.revalidated = false;
        candidate.historicalDirectoryAggregate.recursiveLogicalSize =
            candidate.sizeAtSelection;
        candidate.historicalDirectoryAggregate.newestDescendantWrite =
            candidate.lastWriteTime;
    }
    if (candidate.capturedSafety == LocationSafety::Unknown) {
        candidate.capturedSafety = classifyLocation(candidate.path);
    }
    if (candidate.addedAt == 0) {
        candidate.addedAt = addedAt;
    }
    attachLiveObjectEvidence(candidate, reader);
}

}  // namespace spacelens
