#pragma once

#include "core/Classification.hpp"
#include "core/CleanupReview.hpp"
#include "core/PhysicalStorage.hpp"
#include "core/SafetyPolicy.hpp"
#include "core/Types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spacelens {

enum class ReclaimConfidence {
    Verified,
    Strong,
    Heuristic,
    Unknown
};

enum class ReclaimActionability {
    ActionableWithoutContentJudgment,
    RequiresContentJudgment,
    InformationalOnly
};

enum class ReclaimDisruption {
    Low,
    Moderate,
    Higher,
    Review
};

enum class ReclaimBasis {
    UniqueAllocatedBytes,
    ProviderOwnedUniqueAllocatedBytes,
    SnapshotAllocatedBytes,
    IncompleteHardlinkCoverage,
    Unknown
};

enum class ReclaimConsequence {
    NoneKnown,
    RebuildRequired,
    DependencyReinstallRequired,
    RedownloadRequired,
    ContentJudgmentRequired,
    Unknown
};

[[nodiscard]] const char* toString(ReclaimConfidence value) noexcept;
[[nodiscard]] const char* toString(ReclaimActionability value) noexcept;
[[nodiscard]] const char* toString(ReclaimDisruption value) noexcept;
[[nodiscard]] const char* toString(ReclaimBasis value) noexcept;
[[nodiscard]] const char* toString(ReclaimConsequence value) noexcept;

[[nodiscard]] ReclaimConfidence parseReclaimConfidence(std::string_view text) noexcept;
[[nodiscard]] ReclaimActionability parseReclaimActionability(
    std::string_view text) noexcept;
[[nodiscard]] ReclaimDisruption parseReclaimDisruption(std::string_view text) noexcept;
[[nodiscard]] ReclaimBasis parseReclaimBasis(std::string_view text) noexcept;
[[nodiscard]] ReclaimConsequence parseReclaimConsequence(std::string_view text) noexcept;

struct ReclaimAction {
    std::string kind = "provider_cleanup";
    std::string provider;
    std::string operation;
    bool executionSupported = false;
    bool humanAuthorizationRequired = true;
    ReclaimConsequence consequence = ReclaimConsequence::Unknown;
};

struct ReclaimOwnership {
    std::string provider;
    std::string ecosystem;
    std::vector<std::string> evidence;
    bool authoritative = false;
};

struct ReclaimSizeEvidence {
    ByteSize logicalBytes = 0;
    std::optional<ByteSize> allocatedBytes;
    bool allocationKnown = false;
    HardLinkCoverage hardLinkCoverage = HardLinkCoverage::Unknown;
    std::uint32_t hardLinkCount = 0;
    std::uint32_t observedLinkCount = 0;
    bool sparse = false;
    bool compressed = false;
};

struct ReclaimCandidateEvidence {
    std::wstring path;
    ItemKind kind = ItemKind::Directory;
    StorageIdentity identity{};
    Classification classification{};
    LocationSafety safety = LocationSafety::Unknown;
    Reclaimability reclaimability = Reclaimability::Unknown;
    CandidateStrength strength = CandidateStrength::None;
    ReclaimSizeEvidence size{};
    std::optional<ByteSize> hostReclaimBytes;
    ReclaimBasis basis = ReclaimBasis::Unknown;
    ReclaimConfidence confidence = ReclaimConfidence::Unknown;
    ReclaimActionability actionability = ReclaimActionability::InformationalOnly;
    ReclaimDisruption disruption = ReclaimDisruption::Review;
    ReclaimOwnership ownership{};
    ReclaimAction action{};
    std::vector<std::string> reasonCodes;
    std::string explanation;
    bool snapshotBased = true;
    bool liveRevalidated = false;
    std::uint64_t evidenceTimeTicks = 0;
    std::string evidenceTimeIso;
};

}  // namespace spacelens
