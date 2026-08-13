#pragma once

#include "core/Classification.hpp"
#include "core/CleanupReview.hpp"
#include "core/DirectoryTree.hpp"
#include "core/FileTime.hpp"
#include "core/SafetyPolicy.hpp"
#include "core/Types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spacelens {

/// Combined read-only analysis for human review prioritization.
/// Does NOT include safe_to_delete.
struct ReclaimCandidate {
    std::wstring path;
    ItemKind kind = ItemKind::File;
    ByteSize size_bytes = 0;
    FileTimeTicks activityWriteTime = 0;  // file mtime or dir newest descendant
    std::uint64_t inactiveDays = 0;
    Classification classification{};
    LocationSafety safety = LocationSafety::Unknown;
    Reclaimability reclaimability = Reclaimability::Unknown;
    CandidateStrength strength = CandidateStrength::None;
    std::string activityNote;
    std::string explanation;
};

struct ReclaimQuery {
    ByteSize minSize = 0;
    std::uint64_t olderThanDays = 0;  // 0 = no age filter
    FileTimeTicks nowTicks = 0;      // required when olderThanDays > 0
    bool requireInactive = false;    // alias: apply olderThanDays on activity
    std::optional<StorageCategory> categoryOnly;
    std::size_t limit = 50;
    bool includeFiles = true;
    bool includeDirectories = true;
};

/// Derive reclaimability from classification alone (no age, no safety).
[[nodiscard]] Reclaimability reclaimabilityFor(StorageCategory category) noexcept;

/// Score one item. Protected locations always return strength None.
[[nodiscard]] ReclaimCandidate analyzeItem(
    std::wstring path,
    ItemKind kind,
    ByteSize size,
    FileTimeTicks activityWriteTime,
    Classification classification,
    LocationSafety safety,
    FileTimeTicks nowTicks,
    FileTimeTicks lastAccessTime = 0);

/// Scan tree for reclaim candidates (directories + large files).
[[nodiscard]] std::vector<ReclaimCandidate> findReclaimCandidates(
    const DirectoryTree& tree,
    const ReclaimQuery& query);

}  // namespace spacelens
