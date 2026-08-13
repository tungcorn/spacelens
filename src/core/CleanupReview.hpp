#pragma once

#include "core/Classification.hpp"
#include "core/FileTime.hpp"
#include "core/SafetyPolicy.hpp"
#include "core/Types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spacelens {

enum class ItemKind {
    File,
    Directory,
    ReparseDirectory
};

[[nodiscard]] const char* toString(ItemKind kind) noexcept;

enum class Reclaimability {
    LikelyRegenerable,
    PossiblyRegenerable,
    Unknown,
    NotApplicable
};

enum class CandidateStrength {
    None,
    ReviewOnly,
    Moderate,
    Strong
};

[[nodiscard]] const char* toString(Reclaimability value) noexcept;
[[nodiscard]] const char* toString(CandidateStrength value) noexcept;

enum class CleanupIdentitySource {
    FileId128,
    FileIndex64Fallback,
    Unavailable
};

[[nodiscard]] const char* toString(CleanupIdentitySource source) noexcept;

/// Windows object identity. The source also expresses the identity strength;
/// identity forms are never compared as if they were interchangeable.
struct CleanupIdentity {
    CleanupIdentitySource source = CleanupIdentitySource::Unavailable;
    std::uint64_t volumeSerial = 0;
    std::array<std::uint8_t, 16> fileId128{};
    std::uint64_t fileIndex64 = 0;
};

[[nodiscard]] CleanupIdentity makeFileId128Identity(
    std::uint64_t volumeSerial,
    const std::array<std::uint8_t, 16>& id) noexcept;
[[nodiscard]] CleanupIdentity makeFileIndex64FallbackIdentity(
    std::uint64_t volumeSerial,
    std::uint64_t fileIndex64) noexcept;

[[nodiscard]] bool isStrongIdentity(const CleanupIdentity& identity) noexcept;
[[nodiscard]] bool isIdentityAvailable(const CleanupIdentity& identity) noexcept;
[[nodiscard]] CleanupIdentitySource identityStrength(
    const CleanupIdentity& identity) noexcept;
/// Equality is false for unavailable identities, different forms, or different
/// volume/identifier values.
[[nodiscard]] bool identitiesEqual(const CleanupIdentity& a,
                                   const CleanupIdentity& b) noexcept;
bool operator==(const CleanupIdentity& a, const CleanupIdentity& b) noexcept;
bool operator!=(const CleanupIdentity& a, const CleanupIdentity& b) noexcept;

enum class CleanupEvidenceScope {
    Direct,
    Recursive
};

[[nodiscard]] const char* toString(CleanupEvidenceScope scope) noexcept;

/// Captured/current metadata for the object itself. Directory recursive size
/// belongs in CleanupDirectoryAggregateEvidence.
struct CleanupObjectEvidence {
    bool available = false;
    CleanupIdentity identity{};
    ItemKind kind = ItemKind::File;
    CleanupEvidenceScope sizeScope = CleanupEvidenceScope::Direct;
    ByteSize logicalSize = 0;
    FileTimeTicks lastWriteTime = 0;
    FileTimeTicks lastAccessTime = 0;
    std::uint32_t attributes = 0;
};

/// Historical or current recursive directory evidence. `revalidated` is
/// meaningful for current evidence; a captured aggregate is snapshot data.
struct CleanupDirectoryAggregateEvidence {
    bool available = false;
    bool revalidated = false;
    ByteSize recursiveLogicalSize = 0;
    FileTimeTicks newestDescendantWrite = 0;
};

enum class CleanupObservation {
    Unobserved,
    Present,
    Missing,
    AccessDenied,
    ProbeError
};

[[nodiscard]] const char* toString(CleanupObservation observation) noexcept;

struct CleanupCurrentEvidence {
    bool available = false;
    bool exists = true;
    CleanupObservation observation = CleanupObservation::Unobserved;
    CleanupObjectEvidence objectEvidence{};
    CleanupDirectoryAggregateEvidence directoryAggregate{};
    LocationSafety safety = LocationSafety::Unknown;
};

enum class CleanupValidationState {
    NotValidated,
    Unchanged,
    Missing,
    TypeChanged,
    IdentityChanged,
    IdentityUnavailable,
    Protected,
    Changed,
    DirectUnchangedRecursiveNotRevalidated,
    AccessDenied,
    ProbeError
};

[[nodiscard]] const char* toString(CleanupValidationState state) noexcept;

enum class CleanupValidationReason : std::uint32_t {
    None = 0,
    NoCurrentEvidence = 1U << 0U,
    Missing = 1U << 1U,
    TypeChanged = 1U << 2U,
    IdentityChanged = 1U << 3U,
    IdentityUnavailable = 1U << 4U,
    IdentityFormChanged = 1U << 5U,
    LogicalSizeChanged = 1U << 6U,
    LastWriteChanged = 1U << 7U,
    LastAccessChanged = 1U << 8U,
    AttributesChanged = 1U << 9U,
    SafetyChanged = 1U << 10U,
    ProtectedLocation = 1U << 11U,
    RecursiveChanged = 1U << 12U,
    RecursiveNotRevalidated = 1U << 13U,
    AccessDenied = 1U << 14U,
    ProbeError = 1U << 15U
};

[[nodiscard]] const char* toString(CleanupValidationReason reason) noexcept;
[[nodiscard]] std::vector<std::string> validationReasonNames(
    CleanupValidationReason reasons);

constexpr CleanupValidationReason operator|(CleanupValidationReason a,
                                             CleanupValidationReason b) noexcept
{
    return static_cast<CleanupValidationReason>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

constexpr CleanupValidationReason operator&(CleanupValidationReason a,
                                             CleanupValidationReason b) noexcept
{
    return static_cast<CleanupValidationReason>(
        static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

inline CleanupValidationReason& operator|=(CleanupValidationReason& a,
                                            CleanupValidationReason b) noexcept
{
    a = a | b;
    return a;
}

constexpr CleanupValidationReason operator~(CleanupValidationReason a) noexcept
{
    return static_cast<CleanupValidationReason>(
        ~static_cast<std::uint32_t>(a));
}

[[nodiscard]] constexpr bool hasValidationReason(
    CleanupValidationReason reasons,
    CleanupValidationReason wanted) noexcept
{
    return (static_cast<std::uint32_t>(reasons) &
            static_cast<std::uint32_t>(wanted)) != 0U;
}

enum class CleanupValidationDiffKind {
    Type,
    Identity,
    LogicalSize,
    LastWriteTime,
    LastAccessTime,
    Attributes,
    Safety,
    RecursiveLogicalSize,
    RecursiveNewestDescendantWrite
};

[[nodiscard]] const char* toString(CleanupValidationDiffKind kind) noexcept;

struct CleanupValidationDiff {
    CleanupValidationDiffKind kind = CleanupValidationDiffKind::LogicalSize;
    std::string captured;
    std::string current;
};

struct CleanupValidation {
    CleanupValidationState state = CleanupValidationState::NotValidated;
    CleanupValidationReason reasons = CleanupValidationReason::None;
    std::vector<CleanupValidationDiff> diffs;
    bool objectIdentityMatched = false;
    bool directMetadataUnchanged = false;
    bool recursiveEvidenceRevalidated = false;
};

enum class CleanupItemLifecycle {
    Active,
    Recycled
};

[[nodiscard]] const char* toString(CleanupItemLifecycle lifecycle) noexcept;

enum class CleanupAddResult {
    Added,
    DuplicateUpdated,
    IdentityConflict,
    Invalid
};

[[nodiscard]] const char* toString(CleanupAddResult result) noexcept;

struct CleanupValidationReplacement {
    std::uint64_t id = 0;
    CleanupCurrentEvidence current{};
    FileTimeTicks checkedAt = 0;
    /// Snapshot path used to produce `current`. Empty skips the match check
    /// (single-item refresh). A non-empty path must still identify the same
    /// review row or the whole batch is rejected.
    std::wstring expectedPath;
};

struct CleanupAddOutcome {
    CleanupAddResult result = CleanupAddResult::Invalid;
    std::uint64_t id = 0;
    std::uint64_t conflictingId = 0;

    [[nodiscard]] bool accepted() const noexcept { return id != 0; }
    [[nodiscard]] bool merged() const noexcept
    {
        return result == CleanupAddResult::DuplicateUpdated;
    }
    [[nodiscard]] bool conflicted() const noexcept
    {
        return result == CleanupAddResult::IdentityConflict;
    }
};

/// Snapshot of an item selected for later human review (not deletion).
/// The original scalar fields remain for existing UI/index callers.
struct CleanupCandidate {
    std::uint64_t id = 0;
    std::wstring path;
    ItemKind kind = ItemKind::File;
    ByteSize sizeAtSelection = 0;
    std::uint64_t lastWriteTime = 0;  // FILETIME ticks; 0 unknown
    std::uint32_t attributes = 0;
    Classification classification{};

    CleanupObjectEvidence objectEvidence{};
    CleanupDirectoryAggregateEvidence historicalDirectoryAggregate{};
    LocationSafety capturedSafety = LocationSafety::Unknown;
    Reclaimability capturedReclaimability = Reclaimability::Unknown;
    CandidateStrength capturedCandidateStrength = CandidateStrength::None;
    CleanupCurrentEvidence currentEvidence{};
    CleanupValidation validation{};
    FileTimeTicks validationCheckedAt = 0;
    std::wstring sourceRoot;
    FileTimeTicks addedAt = 0;

    std::string reasonAdded;
    /// "live_scan" (default) or "persistent_index" — planning metadata only.
    std::string source = "live_scan";
    /// Snapshot age when added from an index (0 if live / unknown).
    std::uint64_t indexAgeMs = 0;
    std::string indexIndexedAtIso;
    CleanupItemLifecycle lifecycle = CleanupItemLifecycle::Active;
};

/// Public path helpers used by review duplicate handling and plan overlap.
[[nodiscard]] std::wstring normalizeCleanupPath(std::wstring_view path);
[[nodiscard]] bool isPathAncestorOrEqual(std::wstring_view ancestor,
                                          std::wstring_view path);
[[nodiscard]] bool isStrictPathAncestor(std::wstring_view ancestor,
                                         std::wstring_view path);

[[nodiscard]] CleanupIdentity identityOf(const CleanupCandidate& candidate) noexcept;
[[nodiscard]] CleanupObjectEvidence objectEvidenceOf(
    const CleanupCandidate& candidate) noexcept;
[[nodiscard]] CleanupDirectoryAggregateEvidence historicalAggregateOf(
    const CleanupCandidate& candidate) noexcept;
[[nodiscard]] CleanupValidation validateCleanupCandidate(
    const CleanupCandidate& candidate,
    const CleanupCurrentEvidence& current);

/// Copy current direct object metadata into captured evidence.
/// Directories retain historical recursive aggregates and never invent a
/// recursive size from handle metadata. Returns false when no current probe
/// evidence is present.
[[nodiscard]] bool refreshCapturedEvidence(CleanupCandidate& candidate);

/// In-memory planning queue. Value-type candidates; no filesystem mutation.
class CleanupReview {
public:
    /// Legacy compatibility: returns a stable accepted id, or zero if invalid.
    std::uint64_t add(CleanupCandidate candidate);
    [[nodiscard]] CleanupAddOutcome addDetailed(CleanupCandidate candidate);

    [[nodiscard]] bool removeById(std::uint64_t id);
    [[nodiscard]] bool removeByPath(std::wstring_view path);
    void clear() noexcept;

    [[nodiscard]] bool replaceValidation(std::uint64_t id,
                                         CleanupCurrentEvidence current,
                                         FileTimeTicks checkedAt = 0);
    [[nodiscard]] bool replaceValidationBatch(
        const std::vector<CleanupValidationReplacement>& updates);
    [[nodiscard]] bool refreshEvidence(std::uint64_t id);
    [[nodiscard]] bool setLifecycle(std::uint64_t id,
                                    CleanupItemLifecycle lifecycle);

    /// Next durable id that add() will assign.
    [[nodiscard]] std::uint64_t nextId() const noexcept { return m_nextId; }
    /// Replace the queue and durable id cursor. Persistence load/commit only.
    void resetTo(std::vector<CleanupCandidate> items, std::uint64_t nextId);

    [[nodiscard]] bool containsPath(std::wstring_view path) const;
    [[nodiscard]] std::optional<CleanupCandidate> findById(std::uint64_t id) const;
    [[nodiscard]] std::optional<CleanupCandidate> findByPath(
        std::wstring_view path) const;

    [[nodiscard]] const std::vector<CleanupCandidate>& items() const noexcept
    {
        return m_items;
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_items.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_items.empty(); }
    /// Unique selected logical size after conservative overlap handling.
    [[nodiscard]] ByteSize totalLogicalSize() const noexcept;
    /// Raw selected sum with saturating arithmetic.
    [[nodiscard]] ByteSize rawTotalLogicalSize() const noexcept;

    /// Multi-line UTF-8 report for clipboard / export.
    [[nodiscard]] std::string copyReport() const;

private:
    [[nodiscard]] std::vector<CleanupCandidate>::iterator findIt(
        std::wstring_view path);
    [[nodiscard]] std::vector<CleanupCandidate>::const_iterator findIt(
        std::wstring_view path) const;

    std::vector<CleanupCandidate> m_items;
    std::uint64_t m_nextId = 1;
};

}  // namespace spacelens
