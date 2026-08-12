#include "core/ReclaimAnalysis.hpp"

#include "core/FileTime.hpp"
#include "core/TopKCollector.hpp"

#include <algorithm>
#include <sstream>

namespace spacelens {
namespace {

struct Ranked {
    ReclaimCandidate candidate;
};

struct RankCmp {
    // Min-heap "worse": smaller strength, then smaller size.
    bool operator()(const Ranked& a, const Ranked& b) const
    {
        const auto sa = static_cast<int>(a.candidate.strength);
        const auto sb = static_cast<int>(b.candidate.strength);
        if (sa != sb) {
            return sa < sb;
        }
        if (a.candidate.size_bytes != b.candidate.size_bytes) {
            return a.candidate.size_bytes < b.candidate.size_bytes;
        }
        return a.candidate.path > b.candidate.path;
    }
};

Classification classifyDirFromTree(const DirectoryTree& tree, DirIndex idx)
{
    const auto& node = tree.dir(idx);
    std::vector<std::wstring> children;
    children.reserve(node.children.size() + node.files.size());
    for (const DirIndex c : node.children) {
        children.push_back(tree.dir(c).name);
    }
    for (const FileIndex f : node.files) {
        children.push_back(tree.file(f).name);
    }
    const std::wstring path = tree.pathOfDirectory(idx);
    return classifyDirectory(node.name, path, children.data(), children.size());
}

}  // namespace

const char* toString(Reclaimability value) noexcept
{
    switch (value) {
    case Reclaimability::LikelyRegenerable:
        return "LikelyRegenerable";
    case Reclaimability::PossiblyRegenerable:
        return "PossiblyRegenerable";
    case Reclaimability::Unknown:
        return "Unknown";
    case Reclaimability::NotApplicable:
        return "NotApplicable";
    }
    return "Unknown";
}

const char* toString(CandidateStrength value) noexcept
{
    switch (value) {
    case CandidateStrength::None:
        return "None";
    case CandidateStrength::ReviewOnly:
        return "ReviewOnly";
    case CandidateStrength::Moderate:
        return "Moderate";
    case CandidateStrength::Strong:
        return "Strong";
    }
    return "None";
}

Reclaimability reclaimabilityFor(StorageCategory category) noexcept
{
    switch (category) {
    case StorageCategory::BuildArtifact:
    case StorageCategory::DependencyDirectory:
    case StorageCategory::PackageCache:
    case StorageCategory::IdeCache:
    case StorageCategory::TemporaryData:
        return Reclaimability::LikelyRegenerable;
    case StorageCategory::LogData:
        return Reclaimability::PossiblyRegenerable;
    case StorageCategory::DownloadedAiModel:
    case StorageCategory::Archive:
        return Reclaimability::Unknown;
    case StorageCategory::ApplicationData:
    case StorageCategory::SystemData:
        return Reclaimability::NotApplicable;
    case StorageCategory::UserData:
    case StorageCategory::Unknown:
        return Reclaimability::Unknown;
    }
    return Reclaimability::Unknown;
}

ReclaimCandidate analyzeItem(std::wstring path,
                             ItemKind kind,
                             ByteSize size,
                             FileTimeTicks activityWriteTime,
                             Classification classification,
                             LocationSafety safety,
                             FileTimeTicks nowTicks,
                             FileTimeTicks lastAccessTime)
{
    ReclaimCandidate out;
    out.path = std::move(path);
    out.kind = kind;
    out.size_bytes = size;
    out.activityWriteTime = activityWriteTime;
    out.classification = std::move(classification);
    out.safety = safety;
    out.reclaimability = reclaimabilityFor(out.classification.category);
    out.inactiveDays = ageDays(activityWriteTime, nowTicks);

    if (activityWriteTime != 0 && nowTicks != 0) {
        std::ostringstream note;
        note << "newest write activity ~" << out.inactiveDays << " days ago";
        out.activityNote = note.str();
    } else {
        out.activityNote = "write activity unknown";
    }

    // Protected locations are never reclaim candidates.
    if (isMutationDisallowed(safety) || safety == LocationSafety::Protected) {
        out.strength = CandidateStrength::None;
        out.explanation =
            "Protected system location — SpaceLens will not manage deletion here";
        return out;
    }

    // LastAccessTime alone must never produce Strong.
    // We only use write-based activity for strength. If write is unknown but
    // access is old, stay at ReviewOnly max.
    const bool writeKnown = activityWriteTime != 0 && nowTicks != 0;
    const bool accessOnlyOld =
        !writeKnown && lastAccessTime != 0 && nowTicks != 0 &&
        ageDays(lastAccessTime, nowTicks) >= 90;

    const bool regenerable =
        out.reclaimability == Reclaimability::LikelyRegenerable ||
        out.reclaimability == Reclaimability::PossiblyRegenerable;

    const bool highClass =
        out.classification.confidence == Confidence::High ||
        out.classification.confidence == Confidence::Medium;

    const bool inactiveLong =
        writeKnown && out.inactiveDays >= 90;
    const bool inactiveVeryLong =
        writeKnown && out.inactiveDays >= 180;

    if (out.classification.category == StorageCategory::UserData ||
        out.classification.category == StorageCategory::Unknown ||
        out.classification.category == StorageCategory::DownloadedAiModel ||
        out.classification.category == StorageCategory::Archive ||
        out.classification.category == StorageCategory::ApplicationData ||
        out.classification.category == StorageCategory::SystemData) {
        // User / unknown content: age+size never elevates beyond ReviewOnly.
        out.strength = CandidateStrength::ReviewOnly;
        out.explanation =
            "User or non-regenerable data — large/old alone is not a delete signal";
        if (accessOnlyOld) {
            out.explanation +=
                "; LastAccessTime is advisory and insufficient for Strong";
        }
        return out;
    }

    if (regenerable && highClass && inactiveVeryLong && size >= (100ULL << 20)) {
        out.strength = CandidateStrength::Strong;
        out.explanation =
            "Regenerable classified data with long write inactivity and large size";
    } else if (regenerable && highClass && inactiveLong && size >= (10ULL << 20)) {
        out.strength = CandidateStrength::Moderate;
        out.explanation =
            "Regenerable classified data with write inactivity — review recommended";
    } else if (regenerable && highClass) {
        out.strength = CandidateStrength::ReviewOnly;
        out.explanation =
            "Regenerable classification but activity/size not strong enough";
    } else {
        out.strength = CandidateStrength::ReviewOnly;
        out.explanation = "Insufficient evidence for strong reclaim candidacy";
    }

    // Explicit guard: access-only path cannot be Strong.
    if (!writeKnown && out.strength == CandidateStrength::Strong) {
        out.strength = CandidateStrength::ReviewOnly;
        out.explanation =
            "LastAccessTime alone cannot produce a Strong reclaim recommendation";
    }

    if (safety == LocationSafety::Sensitive &&
        out.strength == CandidateStrength::Strong) {
        out.strength = CandidateStrength::Moderate;
        out.explanation += "; sensitive location caps strength";
    }

    return out;
}

std::vector<ReclaimCandidate> findReclaimCandidates(const DirectoryTree& tree,
                                                    const ReclaimQuery& query)
{
    std::vector<ReclaimCandidate> all;
    if (tree.empty()) {
        return all;
    }

    const FileTimeTicks now = query.nowTicks;
    const std::uint64_t olderDays =
        query.requireInactive ? (query.olderThanDays == 0 ? 90 : query.olderThanDays)
                              : query.olderThanDays;

    auto passesFilters = [&](const ReclaimCandidate& c) -> bool {
        if (c.strength == CandidateStrength::None) {
            return false;
        }
        if (c.size_bytes < query.minSize) {
            return false;
        }
        if (olderDays > 0) {
            if (c.activityWriteTime == 0 || now == 0) {
                return false;
            }
            if (!isOlderThanDays(c.activityWriteTime, now, olderDays)) {
                return false;
            }
        }
        if (query.categoryOnly &&
            c.classification.category != *query.categoryOnly) {
            return false;
        }
        return true;
    };

    if (query.includeDirectories) {
        const std::size_t n = tree.directoryCount();
        for (std::size_t i = 0; i < n; ++i) {
            const DirIndex idx = static_cast<DirIndex>(i);
            // Skip scan root as a reclaim target by default (too broad).
            if (idx == tree.root()) {
                continue;
            }
            const auto& node = tree.dir(idx);
            if (node.recursiveSize < query.minSize && query.minSize > 0) {
                continue;
            }
            const std::wstring path = tree.pathOfDirectory(idx);
            const LocationSafety safety = classifyLocation(path);
            Classification cls = classifyDirFromTree(tree, idx);
            FileTimeTicks activity = node.newestDescendantWrite;
            auto cand = analyzeItem(path, ItemKind::Directory, node.recursiveSize,
                                    activity, std::move(cls), safety, now, 0);
            if (passesFilters(cand)) {
                all.push_back(std::move(cand));
            }
        }
    }

    if (query.includeFiles) {
        const std::size_t n = tree.fileCount();
        for (std::size_t i = 0; i < n; ++i) {
            const FileIndex idx = static_cast<FileIndex>(i);
            const auto& file = tree.file(idx);
            if (file.size < query.minSize && query.minSize > 0) {
                continue;
            }
            const std::wstring path = tree.pathOfFile(idx);
            const LocationSafety safety = classifyLocation(path);
            Classification cls = classifyFile(file.name, path);
            auto cand = analyzeItem(path, ItemKind::File, file.size,
                                    file.lastWriteTime, std::move(cls), safety,
                                    now, file.lastAccessTime);
            if (passesFilters(cand)) {
                all.push_back(std::move(cand));
            }
        }
    }

    // Sort: Strong first, then size desc.
    std::sort(all.begin(), all.end(), [](const ReclaimCandidate& a,
                                         const ReclaimCandidate& b) {
        const auto sa = static_cast<int>(a.strength);
        const auto sb = static_cast<int>(b.strength);
        if (sa != sb) {
            return sa > sb;
        }
        if (a.size_bytes != b.size_bytes) {
            return a.size_bytes > b.size_bytes;
        }
        return a.path < b.path;
    });

    if (all.size() > query.limit) {
        all.resize(query.limit);
    }
    return all;
}

}  // namespace spacelens
