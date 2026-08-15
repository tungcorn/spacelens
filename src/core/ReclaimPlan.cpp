#include "core/ReclaimPlan.hpp"

#include "core/Classification.hpp"
#include "core/DirectoryTree.hpp"
#include "core/Json.hpp"
#include "core/ReclaimAnalysis.hpp"
#include "core/ReclaimProvider.hpp"
#include "core/SafetyPolicy.hpp"
#include "core/ScanEngine.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexRefresh.hpp"
#include "core/index/IndexSnapshot.hpp"
#include "core/index/IndexStore.hpp"
#include "core/index/PhysicalAccounting.hpp"
#include "platform/windows/FileIdentity.hpp"
#include "platform/windows/WindowsFileEnumerator.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace spacelens {
namespace {

constexpr ByteSize kReviewOnlyMinBytes = 1024ULL * 1024ULL;
constexpr int kSchemaVersion = 1;

FileTimeTicks nowFileTime()
{
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

int actionabilityRank(ReclaimActionability v) noexcept
{
    switch (v) {
    case ReclaimActionability::ActionableWithoutContentJudgment:
        return 0;
    case ReclaimActionability::RequiresContentJudgment:
        return 1;
    case ReclaimActionability::InformationalOnly:
        return 2;
    }
    return 3;
}

int confidenceRank(ReclaimConfidence v) noexcept
{
    switch (v) {
    case ReclaimConfidence::Verified:
        return 0;
    case ReclaimConfidence::Strong:
        return 1;
    case ReclaimConfidence::Heuristic:
        return 2;
    case ReclaimConfidence::Unknown:
        return 3;
    }
    return 4;
}

int disruptionRank(ReclaimDisruption v) noexcept
{
    switch (v) {
    case ReclaimDisruption::Low:
        return 0;
    case ReclaimDisruption::Moderate:
        return 1;
    case ReclaimDisruption::Higher:
        return 2;
    case ReclaimDisruption::Review:
        return 3;
    }
    return 4;
}

bool pathLessIgnoreCase(std::wstring_view a, std::wstring_view b)
{
    const std::size_t n = (std::min)(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const wchar_t ca = static_cast<wchar_t>(std::towlower(a[i]));
        const wchar_t cb = static_cast<wchar_t>(std::towlower(b[i]));
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

bool betterRank(const ReclaimCandidateEvidence& a,
                const ReclaimCandidateEvidence& b)
{
    const int aa = actionabilityRank(a.actionability);
    const int ba = actionabilityRank(b.actionability);
    if (aa != ba) {
        return aa < ba;
    }
    const int ac = confidenceRank(a.confidence);
    const int bc = confidenceRank(b.confidence);
    if (ac != bc) {
        return ac < bc;
    }
    const int ad = disruptionRank(a.disruption);
    const int bd = disruptionRank(b.disruption);
    if (ad != bd) {
        return ad < bd;
    }
    const ByteSize ah = a.hostReclaimBytes.value_or(0);
    const ByteSize bh = b.hostReclaimBytes.value_or(0);
    if (ah != bh) {
        return ah > bh;
    }
    return pathLessIgnoreCase(a.path, b.path);
}

bool overlapping(const ReclaimCandidateEvidence& a,
                 const ReclaimCandidateEvidence& b)
{
    return pathIsUnderNormalized(a.path, b.path) ||
           pathIsUnderNormalized(b.path, a.path);
}

LocationSafety parseSafetyStored(std::string_view text)
{
    if (text == "Protected") {
        return LocationSafety::Protected;
    }
    if (text == "Sensitive") {
        return LocationSafety::Sensitive;
    }
    if (text == "Ordinary") {
        return LocationSafety::Ordinary;
    }
    return LocationSafety::Unknown;
}

Reclaimability parseReclaimStored(std::string_view text)
{
    if (text == "LikelyRegenerable") {
        return Reclaimability::LikelyRegenerable;
    }
    if (text == "PossiblyRegenerable") {
        return Reclaimability::PossiblyRegenerable;
    }
    if (text == "NotApplicable") {
        return Reclaimability::NotApplicable;
    }
    return Reclaimability::Unknown;
}

CandidateStrength parseStrengthStored(std::string_view text)
{
    if (text == "Strong") {
        return CandidateStrength::Strong;
    }
    if (text == "Moderate") {
        return CandidateStrength::Moderate;
    }
    if (text == "ReviewOnly") {
        return CandidateStrength::ReviewOnly;
    }
    return CandidateStrength::None;
}

Confidence parseClassConfidence(std::string_view text)
{
    if (text == "High") {
        return Confidence::High;
    }
    if (text == "Medium") {
        return Confidence::Medium;
    }
    return Confidence::Low;
}

bool regenerableCategory(StorageCategory category)
{
    return category == StorageCategory::BuildArtifact ||
           category == StorageCategory::PackageCache ||
           category == StorageCategory::IdeCache ||
           category == StorageCategory::TemporaryData ||
           category == StorageCategory::LogData;
}

std::vector<std::wstring> treeChildNames(const DirectoryTree& tree, DirIndex di)
{
    std::vector<std::wstring> names;
    const auto& node = tree.dir(di);
    names.reserve(node.children.size() + node.files.size());
    for (const DirIndex c : node.children) {
        names.push_back(tree.dir(c).name);
    }
    for (const FileIndex f : node.files) {
        names.push_back(tree.file(f).name);
    }
    return names;
}

std::vector<std::wstring> treeSiblingNames(const DirectoryTree& tree, DirIndex di)
{
    const auto& node = tree.dir(di);
    if (node.parent == InvalidDirIndex) {
        return {};
    }
    return treeChildNames(tree, node.parent);
}

void applyProviderOrHeuristic(ReclaimCandidateEvidence& cand,
                              const ReclaimProviderContext& ctx,
                              const std::wstring* childNames, std::size_t childCount,
                              const std::wstring* siblingNames,
                              std::size_t siblingCount)
{
    ReclaimProbeInput in;
    in.path = cand.path;
    in.name = cand.path;
    const auto slash = cand.path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash + 1 < cand.path.size()) {
        in.name = cand.path.substr(slash + 1);
    }
    in.kind = cand.kind;
    in.classification = cand.classification;
    in.safety = cand.safety;
    in.childNames = childNames;
    in.childCount = childCount;
    in.siblingNames = siblingNames;
    in.siblingCount = siblingCount;

    if (auto hit = classifyReclaimOwnership(in, ctx)) {
        cand.ownership = hit->ownership;
        cand.action = hit->action;
        cand.actionability = hit->actionability;
        cand.disruption = hit->disruption;
        cand.confidence = hit->confidence;
        cand.reasonCodes.insert(cand.reasonCodes.end(), hit->reasonCodes.begin(),
                                hit->reasonCodes.end());
        cand.basis = ReclaimBasis::ProviderOwnedUniqueAllocatedBytes;
        return;
    }

    cand.action.kind = "none";
    cand.action.executionSupported = false;
    cand.action.humanAuthorizationRequired = true;
    if (isProtectedFromReclaim(cand.safety)) {
        cand.actionability = ReclaimActionability::InformationalOnly;
        cand.disruption = ReclaimDisruption::Review;
        cand.confidence = ReclaimConfidence::Unknown;
        cand.action.consequence = ReclaimConsequence::Unknown;
        cand.reasonCodes.emplace_back("protected_location");
        return;
    }
    if (isReviewOnlyClassification(cand.classification.category, cand.path,
                                   in.name)) {
        cand.actionability = ReclaimActionability::RequiresContentJudgment;
        cand.disruption = ReclaimDisruption::Review;
        cand.confidence = ReclaimConfidence::Heuristic;
        cand.action.consequence = ReclaimConsequence::ContentJudgmentRequired;
        cand.reasonCodes.emplace_back("content_judgment_required");
        return;
    }
    if (regenerableCategory(cand.classification.category)) {
        cand.actionability =
            ReclaimActionability::ActionableWithoutContentJudgment;
        cand.disruption = ReclaimDisruption::Moderate;
        cand.confidence = ReclaimConfidence::Heuristic;
        cand.action.kind = "regenerable_classification";
        cand.action.consequence = ReclaimConsequence::RebuildRequired;
        cand.basis = ReclaimBasis::UniqueAllocatedBytes;
        cand.reasonCodes.emplace_back("classified_regenerable");
        return;
    }
    cand.actionability = ReclaimActionability::RequiresContentJudgment;
    cand.disruption = ReclaimDisruption::Review;
    cand.confidence = ReclaimConfidence::Heuristic;
    cand.action.consequence = ReclaimConsequence::ContentJudgmentRequired;
    cand.reasonCodes.emplace_back("unverified_ownership");
}

void applySizeAndCoverage(ReclaimCandidateEvidence& cand,
                          const UniqueAllocation& sum)
{
    cand.size.allocationKnown = sum.allAllocationKnown;
    cand.size.allocatedBytes = sum.uniqueAllocatedBytes;
    cand.size.hardLinkCoverage = sum.coverage;
    if (sum.exactReclaimBytes.has_value()) {
        cand.hostReclaimBytes = sum.exactReclaimBytes;
        if (cand.basis == ReclaimBasis::Unknown) {
            cand.basis = ReclaimBasis::UniqueAllocatedBytes;
        }
    } else {
        cand.hostReclaimBytes.reset();
        if (sum.coverage == HardLinkCoverage::Incomplete) {
            cand.basis = ReclaimBasis::IncompleteHardlinkCoverage;
            cand.reasonCodes.emplace_back("incomplete_hardlink_coverage");
        } else {
            cand.basis = ReclaimBasis::Unknown;
            cand.reasonCodes.emplace_back("allocation_unknown");
        }
    }
    if (!sum.allAllocationKnown) {
        cand.reasonCodes.emplace_back("allocation_incomplete");
    }
    if (isProtectedFromReclaim(cand.safety) ||
        cand.actionability !=
            ReclaimActionability::ActionableWithoutContentJudgment ||
        cand.size.hardLinkCoverage != HardLinkCoverage::Complete ||
        !cand.hostReclaimBytes.has_value()) {
        if (cand.actionability ==
                ReclaimActionability::ActionableWithoutContentJudgment &&
            (cand.size.hardLinkCoverage != HardLinkCoverage::Complete ||
             !cand.hostReclaimBytes.has_value())) {
            cand.actionability = ReclaimActionability::RequiresContentJudgment;
            cand.reasonCodes.emplace_back("incomplete_evidence");
        }
    }
}

void buildExplanation(ReclaimCandidateEvidence& cand)
{
    std::ostringstream os;
    os << cand.classification.reason;
    if (!cand.ownership.provider.empty()) {
        os << " Provider=" << cand.ownership.provider << ".";
    }
    os << " host_reclaim_bytes is exact unique allocated with complete "
          "hard-link coverage only.";
    if (cand.action.executionSupported) {
        cand.action.executionSupported = false;
    }
    cand.explanation = os.str();
}

struct IdentSample {
    StorageIdentity identity;
    std::wstring path;
    std::optional<ByteSize> allocatedBytes;
    bool allocationKnown = false;
    std::uint32_t filesystemLinks = 0;
    bool sparse = false;
    bool compressed = false;
};

struct IdentityIndex {
    std::vector<IdentSample> files;
    std::unordered_map<StorageIdentity, std::uint32_t, StorageIdentityHash>
        observedInIndex;
};

void addFileSample(IdentityIndex& idx, IdentSample sample)
{
    if (sample.identity.valid()) {
        ++idx.observedInIndex[sample.identity];
    }
    idx.files.push_back(std::move(sample));
}

UniqueAllocation summarizeUnder(const IdentityIndex& idx,
                                std::wstring_view ancestor)
{
    std::unordered_map<StorageIdentity, IdentityAllocation, StorageIdentityHash>
        grouped;
    std::vector<IdentityAllocation> unknown;
    for (const auto& f : idx.files) {
        if (!pathIsUnderNormalized(f.path, ancestor)) {
            continue;
        }
        if (!f.identity.valid()) {
            IdentityAllocation u;
            u.allocatedBytes = f.allocatedBytes;
            u.allocationKnown = f.allocationKnown;
            u.filesystemLinks = f.filesystemLinks;
            u.observedInCandidate = 1;
            u.sparse = f.sparse;
            u.compressed = f.compressed;
            unknown.push_back(u);
            continue;
        }
        auto& slot = grouped[f.identity];
        slot.identity = f.identity;
        if (f.allocationKnown && f.allocatedBytes.has_value()) {
            slot.allocatedBytes = f.allocatedBytes;
            slot.allocationKnown = true;
        }
        if (f.filesystemLinks > slot.filesystemLinks) {
            slot.filesystemLinks = f.filesystemLinks;
        }
        ++slot.observedInCandidate;
        slot.sparse = slot.sparse || f.sparse;
        slot.compressed = slot.compressed || f.compressed;
    }
    std::vector<IdentityAllocation> items;
    items.reserve(grouped.size() + unknown.size());
    for (auto& [id, alloc] : grouped) {
        const auto it = idx.observedInIndex.find(id);
        alloc.observedInIndex =
            it == idx.observedInIndex.end() ? alloc.observedInCandidate
                                            : it->second;
        items.push_back(alloc);
    }
    items.insert(items.end(), unknown.begin(), unknown.end());
    return summarizeIdentities(items);
}

void finishCandidate(ReclaimCandidateEvidence& cand, const IdentityIndex& idx,
                     const ReclaimProviderContext& ctx,
                     const std::wstring* childNames, std::size_t childCount,
                     const std::wstring* siblingNames, std::size_t siblingCount)
{
    applyProviderOrHeuristic(cand, ctx, childNames, childCount, siblingNames,
                             siblingCount);
    applySizeAndCoverage(cand, summarizeUnder(idx, cand.path));
    if (cand.kind == ItemKind::File) {
        for (const auto& f : idx.files) {
            if (pathIsUnderNormalized(f.path, cand.path) &&
                pathIsUnderNormalized(cand.path, f.path)) {
                cand.size.hardLinkCount = f.filesystemLinks;
                cand.size.observedLinkCount = 1;
                if (f.identity.valid()) {
                    const auto it = idx.observedInIndex.find(f.identity);
                    if (it != idx.observedInIndex.end()) {
                        cand.size.observedLinkCount = it->second;
                    }
                }
                cand.size.sparse = f.sparse;
                cand.size.compressed = f.compressed;
                cand.identity = f.identity;
                break;
            }
        }
    }
    buildExplanation(cand);
}

bool shouldKeepDirectory(const ReclaimCandidateEvidence& cand, ByteSize logical)
{
    if (isProtectedFromReclaim(cand.safety)) {
        return logical >= kReviewOnlyMinBytes;
    }
    if (!cand.ownership.provider.empty()) {
        return true;
    }
    if (regenerableCategory(cand.classification.category)) {
        return true;
    }
    if (cand.classification.category == StorageCategory::DependencyDirectory) {
        return true;
    }
    return logical >= kReviewOnlyMinBytes;
}

bool shouldKeepFile(const ReclaimCandidateEvidence& cand, ByteSize logical)
{
    if (logical < kReviewOnlyMinBytes) {
        return false;
    }
    return isReviewOnlyClassification(cand.classification.category, cand.path,
                                      cand.path) ||
           cand.classification.category == StorageCategory::UserData ||
           cand.classification.category == StorageCategory::Archive ||
           cand.classification.category == StorageCategory::DownloadedAiModel;
}

std::vector<ReclaimCandidateEvidence> suppressOverlap(
    std::vector<ReclaimCandidateEvidence> ranked)
{
    std::vector<ReclaimCandidateEvidence> kept;
    kept.reserve(ranked.size());
    for (auto& cand : ranked) {
        bool hide = false;
        for (const auto& prev : kept) {
            if (overlapping(prev, cand)) {
                hide = true;
                break;
            }
        }
        if (!hide) {
            kept.push_back(std::move(cand));
        }
    }
    return kept;
}

void limitList(std::vector<ReclaimCandidateEvidence>& list, std::size_t limit)
{
    if (list.size() > limit) {
        list.resize(limit);
    }
}

void revalidateOne(ReclaimCandidateEvidence& cand, std::stop_token stop)
{
    if (stop.stop_requested()) {
        return;
    }
    cand.liveRevalidated = true;
    const auto live = queryFileIdentity(cand.path);
    if (!live) {
        cand.reasonCodes.emplace_back("live_missing_or_inaccessible");
        if (cand.confidence == ReclaimConfidence::Verified) {
            cand.confidence = ReclaimConfidence::Strong;
        }
        return;
    }
    if (cand.identity.valid() &&
        (live->fileId != cand.identity.fileId ||
         (live->volumeSerial != 0 && cand.identity.volumeSerial != 0 &&
          live->volumeSerial != cand.identity.volumeSerial))) {
        cand.reasonCodes.emplace_back("live_identity_mismatch");
        cand.confidence = ReclaimConfidence::Heuristic;
        cand.actionability = ReclaimActionability::RequiresContentJudgment;
        return;
    }
    if (cand.kind == ItemKind::File) {
        if (!live->allocationKnown || !live->allocatedBytes.has_value()) {
            cand.size.allocationKnown = false;
            cand.hostReclaimBytes.reset();
            cand.reasonCodes.emplace_back("live_allocation_unknown");
            return;
        }
        cand.size.allocatedBytes = live->allocatedBytes;
        cand.size.allocationKnown = true;
        cand.size.hardLinkCount = live->numberOfLinks;
        cand.size.sparse = live->sparse;
        cand.size.compressed = live->compressed;
        if (live->numberOfLinks <= 1 && cand.size.hardLinkCoverage ==
                                            HardLinkCoverage::Complete) {
            cand.hostReclaimBytes = live->allocatedBytes;
        } else if (live->numberOfLinks > 1) {
            cand.size.hardLinkCoverage = HardLinkCoverage::Incomplete;
            cand.hostReclaimBytes.reset();
            cand.basis = ReclaimBasis::IncompleteHardlinkCoverage;
            cand.reasonCodes.emplace_back("live_hardlink_unconfirmed");
        }
    }
    const bool verified =
        cand.ownership.authoritative &&
        cand.size.allocationKnown &&
        cand.size.hardLinkCoverage == HardLinkCoverage::Complete &&
        cand.hostReclaimBytes.has_value() &&
        !isProtectedFromReclaim(cand.safety) &&
        cand.actionability ==
            ReclaimActionability::ActionableWithoutContentJudgment;
    if (verified) {
        cand.confidence = ReclaimConfidence::Verified;
        cand.reasonCodes.emplace_back("live_revalidated");
    }
}

void revalidateBounded(std::vector<ReclaimCandidateEvidence>& a,
                       std::vector<ReclaimCandidateEvidence>& b,
                       std::vector<ReclaimCandidateEvidence>& c,
                       std::stop_token stop)
{
    for (auto* list : {&a, &b, &c}) {
        for (auto& cand : *list) {
            revalidateOne(cand, stop);
            if (stop.stop_requested()) {
                return;
            }
        }
    }
}

void assembleReport(ReclaimPlanReport& report,
                    std::vector<ReclaimCandidateEvidence> candidates,
                    const ReclaimPlanRequest& request)
{
    std::sort(candidates.begin(), candidates.end(), betterRank);
    auto kept = suppressOverlap(std::move(candidates));

    ByteSize exact = 0;
    bool exactAny = false;
    ByteSize actionableSum = 0;
    bool actionableAny = false;
    HardLinkCoverage overall = HardLinkCoverage::Complete;
    bool sawIncomplete = false;
    bool sawUnknown = false;
    bool sawComplete = false;

    std::vector<ReclaimCandidateEvidence> actionable;
    std::vector<ReclaimCandidateEvidence> review;
    for (auto& cand : kept) {
        if (cand.size.hardLinkCoverage == HardLinkCoverage::Incomplete) {
            sawIncomplete = true;
        } else if (cand.size.hardLinkCoverage == HardLinkCoverage::Unknown) {
            sawUnknown = true;
        } else {
            sawComplete = true;
        }
        if (cand.hostReclaimBytes.has_value()) {
            exactAny = true;
            (void)addSaturating(exact, *cand.hostReclaimBytes);
        }
        const bool isActionable =
            cand.actionability ==
                ReclaimActionability::ActionableWithoutContentJudgment &&
            !isProtectedFromReclaim(cand.safety);
        if (isActionable) {
            if (cand.hostReclaimBytes.has_value()) {
                actionableAny = true;
                (void)addSaturating(actionableSum, *cand.hostReclaimBytes);
            }
            actionable.push_back(std::move(cand));
        } else {
            review.push_back(std::move(cand));
        }
    }

    if (sawUnknown && !sawComplete && !sawIncomplete) {
        overall = HardLinkCoverage::Unknown;
    } else if (sawIncomplete || sawUnknown) {
        overall = HardLinkCoverage::Incomplete;
    }

    std::vector<ReclaimCandidateEvidence> selected;
    ByteSize selectedSum = 0;
    bool selectedAny = false;
    if (request.targetFreeBytes.has_value()) {
        for (const auto& cand : actionable) {
            if (!cand.hostReclaimBytes.has_value() ||
                *cand.hostReclaimBytes == 0) {
                continue;
            }
            selected.push_back(cand);
            selectedAny = true;
            (void)addSaturating(selectedSum, *cand.hostReclaimBytes);
            if (selectedSum >= *request.targetFreeBytes) {
                break;
            }
        }
        report.targetMet = selectedSum >= *request.targetFreeBytes;
    }

    report.actionableHostReclaimBytes =
        actionableAny ? std::optional<ByteSize>(actionableSum) : std::nullopt;
    report.exactHostReclaimBytes =
        exactAny ? std::optional<ByteSize>(exact) : std::nullopt;
    report.selectedHostReclaimBytes =
        selectedAny ? std::optional<ByteSize>(selectedSum) : std::nullopt;
    report.overallCoverage = overall;
    report.selected = std::move(selected);
    limitList(actionable, request.limit);
    limitList(review, request.limit);
    report.actionable = std::move(actionable);
    report.reviewOnly = std::move(review);
}

void appendOptionalBytes(std::ostringstream& os, const char* key,
                         const std::optional<ByteSize>& value, bool comma)
{
    if (comma) {
        os << ',';
    }
    os << '"' << key << "\":";
    if (value.has_value()) {
        os << jsonUInt(*value);
    } else {
        os << "null";
    }
}

void appendStringArray(std::ostringstream& os, const char* key,
                       const std::vector<std::string>& values)
{
    os << '"' << key << "\":[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            os << ',';
        }
        os << jsonString(values[i]);
    }
    os << ']';
}

void appendCandidate(std::ostringstream& os, const ReclaimCandidateEvidence& c)
{
    os << '{'
       << "\"path\":" << jsonString(c.path) << ','
       << "\"kind\":" << jsonString(toString(c.kind)) << ','
       << "\"classification\":" << jsonString(toString(c.classification.category))
       << ','
       << "\"classification_confidence\":"
       << jsonString(toString(c.classification.confidence)) << ','
       << "\"rule_id\":" << jsonString(c.classification.ruleId) << ','
       << "\"ecosystem\":" << jsonString(c.classification.ecosystem) << ','
       << "\"location_safety\":" << jsonString(toString(c.safety)) << ','
       << "\"reclaimability\":" << jsonString(toString(c.reclaimability)) << ','
       << "\"candidate_strength\":" << jsonString(toString(c.strength)) << ',';
    os << "\"logical_bytes\":" << jsonUInt(c.size.logicalBytes);
    appendOptionalBytes(os, "allocated_bytes", c.size.allocatedBytes, true);
    os << ",\"allocation_known\":" << jsonBool(c.size.allocationKnown)
       << ",\"hard_link_count\":" << jsonUInt(c.size.hardLinkCount)
       << ",\"observed_link_count\":" << jsonUInt(c.size.observedLinkCount)
       << ",\"hard_link_coverage\":"
       << jsonString(toString(c.size.hardLinkCoverage))
       << ",\"sparse\":" << jsonBool(c.size.sparse)
       << ",\"compressed\":" << jsonBool(c.size.compressed);
    appendOptionalBytes(os, "host_reclaim_bytes", c.hostReclaimBytes, true);
    os << ",\"reclaim_basis\":" << jsonString(toString(c.basis))
       << ",\"reclaim_confidence\":" << jsonString(toString(c.confidence))
       << ",\"actionability\":" << jsonString(toString(c.actionability))
       << ",\"disruption\":" << jsonString(toString(c.disruption))
       << ",\"ownership\":{"
       << "\"provider\":" << jsonString(c.ownership.provider) << ','
       << "\"ecosystem\":" << jsonString(c.ownership.ecosystem) << ',';
    appendStringArray(os, "evidence", c.ownership.evidence);
    os << ",\"authoritative\":" << jsonBool(c.ownership.authoritative) << "},"
       << "\"action\":{"
       << "\"kind\":" << jsonString(c.action.kind) << ','
       << "\"provider\":" << jsonString(c.action.provider) << ','
       << "\"operation\":" << jsonString(c.action.operation) << ','
       << "\"execution_supported\":" << jsonBool(false) << ','
       << "\"human_authorization_required\":"
       << jsonBool(c.action.humanAuthorizationRequired) << ','
       << "\"consequence\":" << jsonString(toString(c.action.consequence))
       << "},";
    appendStringArray(os, "reason_codes", c.reasonCodes);
    os << ",\"explanation\":" << jsonString(c.explanation)
       << ",\"snapshot_based\":" << jsonBool(c.snapshotBased)
       << ",\"live_revalidated\":" << jsonBool(c.liveRevalidated)
       << ",\"volume_serial\":" << jsonUInt(c.identity.volumeSerial)
       << ",\"file_id\":" << jsonUInt(c.identity.fileId) << '}';
}

void appendCandidateArray(std::ostringstream& os, const char* key,
                          const std::vector<ReclaimCandidateEvidence>& items)
{
    os << '"' << key << "\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            os << ',';
        }
        appendCandidate(os, items[i]);
    }
    os << ']';
}

ReclaimPlanReport failReport(const ReclaimPlanRequest& request, std::string error,
                             ReclaimPlanSource source)
{
    ReclaimPlanReport report;
    report.ok = false;
    report.error = std::move(error);
    report.state = "failed";
    report.root = request.root;
    report.sourceUsed = source;
    report.targetFreeBytes = request.targetFreeBytes;
    return report;
}

ReclaimPlanReport buildFromLive(const ReclaimPlanRequest& request,
                                std::stop_token stop)
{
    WindowsFileEnumerator enumerator;
    ScanEngine engine(enumerator);
    ScanOptions options;
    auto scan = engine.scan(request.root, options, stop);
    if (scan.state == ScanState::Cancelled || stop.stop_requested()) {
        auto report = failReport(request, "cancelled", ReclaimPlanSource::LiveScan);
        report.state = "cancelled";
        return report;
    }
    if (scan.state != ScanState::Completed || scan.tree.empty()) {
        return failReport(request, "scan_failed", ReclaimPlanSource::LiveScan);
    }

    ReclaimPlanReport report;
    report.root = request.root;
    report.sourceUsed = ReclaimPlanSource::LiveScan;
    report.targetFreeBytes = request.targetFreeBytes;
    report.physicalAccounting = true;

    const auto ctx = probeProviderLocations(stop);
    report.providerProbeDetail = ctx.probeDetail;
    if (stop.stop_requested()) {
        report.ok = false;
        report.error = "cancelled";
        report.state = "cancelled";
        return report;
    }

    IdentityIndex idx;
    idx.files.reserve(scan.tree.fileCount());
    const FileTimeTicks now =
        request.nowTicks != 0 ? request.nowTicks : nowFileTime();

    for (std::size_t i = 0; i < scan.tree.fileCount(); ++i) {
        if (stop.stop_requested()) {
            report.ok = false;
            report.error = "cancelled";
            report.state = "cancelled";
            return report;
        }
        const FileIndex fi = static_cast<FileIndex>(i);
        const auto& file = scan.tree.file(fi);
        const std::wstring path = scan.tree.pathOfFile(fi);
        IdentSample sample;
        sample.path = path;
        if (auto id = queryFileIdentity(path)) {
            sample.identity.fileId = id->fileId;
            sample.identity.volumeSerial = id->volumeSerial;
            sample.allocatedBytes = id->allocatedBytes;
            sample.allocationKnown = id->allocationKnown;
            sample.filesystemLinks = id->numberOfLinks;
            sample.sparse = id->sparse;
            sample.compressed = id->compressed;
        }
        addFileSample(idx, std::move(sample));
        (void)file;
    }

    std::vector<ReclaimCandidateEvidence> candidates;
    const DirIndex root = scan.tree.root();
    for (std::size_t i = 0; i < scan.tree.directoryCount(); ++i) {
        if (stop.stop_requested()) {
            report.ok = false;
            report.error = "cancelled";
            report.state = "cancelled";
            return report;
        }
        const DirIndex di = static_cast<DirIndex>(i);
        if (di == root) {
            continue;
        }
        const auto& node = scan.tree.dir(di);
        const std::wstring path = scan.tree.pathOfDirectory(di);
        ReclaimCandidateEvidence cand;
        cand.path = path;
        cand.kind = ItemKind::Directory;
        cand.classification = classifyDirectoryFromTree(scan.tree, di);
        cand.safety = classifyLocation(path);
        cand.size.logicalBytes = node.recursiveSize;
        cand.snapshotBased = false;
        cand.evidenceTimeTicks = now;
        const auto reclaim = analyzeItem(
            path, ItemKind::Directory, node.recursiveSize,
            node.newestDescendantWrite, cand.classification, cand.safety, now, 0);
        cand.reclaimability = reclaim.reclaimability;
        cand.strength = reclaim.strength;
        if (auto id = queryFileIdentity(path)) {
            cand.identity.fileId = id->fileId;
            cand.identity.volumeSerial = id->volumeSerial;
        }
        const auto children = treeChildNames(scan.tree, di);
        const auto siblings = treeSiblingNames(scan.tree, di);
        finishCandidate(cand, idx, ctx, children.data(), children.size(),
                        siblings.data(), siblings.size());
        if (shouldKeepDirectory(cand, node.recursiveSize)) {
            candidates.push_back(std::move(cand));
        }
    }

    for (std::size_t i = 0; i < scan.tree.fileCount(); ++i) {
        const FileIndex fi = static_cast<FileIndex>(i);
        const auto& file = scan.tree.file(fi);
        const std::wstring path = scan.tree.pathOfFile(fi);
        ReclaimCandidateEvidence cand;
        cand.path = path;
        cand.kind = ItemKind::File;
        cand.classification = classifyFile(file.name, path);
        cand.safety = classifyLocation(path);
        cand.size.logicalBytes = file.size;
        cand.snapshotBased = false;
        cand.evidenceTimeTicks = now;
        const auto reclaim = analyzeItem(path, ItemKind::File, file.size,
                                         file.lastWriteTime, cand.classification,
                                         cand.safety, now, file.lastAccessTime);
        cand.reclaimability = reclaim.reclaimability;
        cand.strength = reclaim.strength;
        if (!shouldKeepFile(cand, file.size)) {
            continue;
        }
        finishCandidate(cand, idx, ctx, nullptr, 0, nullptr, 0);
        candidates.push_back(std::move(cand));
    }

    assembleReport(report, std::move(candidates), request);
    revalidateBounded(report.actionable, report.reviewOnly, report.selected,
                      stop);
    if (stop.stop_requested()) {
        report.ok = false;
        report.error = "cancelled";
        report.state = "cancelled";
        return report;
    }
    {
        ByteSize selectedSum = 0;
        bool selectedAny = false;
        for (const auto& cand : report.selected) {
            if (cand.hostReclaimBytes.has_value()) {
                selectedAny = true;
                (void)addSaturating(selectedSum, *cand.hostReclaimBytes);
            }
        }
        report.selectedHostReclaimBytes =
            selectedAny ? std::optional<ByteSize>(selectedSum) : std::nullopt;
        if (request.targetFreeBytes.has_value()) {
            report.targetMet = selectedSum >= *request.targetFreeBytes;
        }
    }
    report.ok = true;
    report.state = "completed";
    return report;
}

ReclaimPlanReport buildFromIndex(const ReclaimPlanRequest& request,
                                 std::stop_token stop)
{
    const auto loc = locateIndex(request.root);
    if (!indexDatabaseExists(loc)) {
        return failReport(request, "index_not_found",
                          ReclaimPlanSource::PersistentIndex);
    }

    try {
        auto store = IndexStore::openRead(loc);
        if (!store.schemaSupported()) {
            return failReport(request, "unsupported_schema",
                              ReclaimPlanSource::PersistentIndex);
        }
        if (!indexHasPhysicalAccounting(store.db())) {
            return failReport(request, "physical_accounting_unavailable",
                              ReclaimPlanSource::PersistentIndex);
        }
        const auto meta = store.readRootMeta();
        if (!meta || meta->status != IndexStatus::Ready) {
            return failReport(request, "index_not_ready",
                              ReclaimPlanSource::PersistentIndex);
        }

        const FileTimeTicks now =
            request.nowTicks != 0 ? request.nowTicks : nowFileTime();
        IndexPublishMetadata pub;
        pub.rootIndexedAtTicks = meta->indexedAtTicks;
        pub.rootIndexedAtIso = meta->indexedAtIso;
        if (auto cp = readRefreshCheckpoint(store.db())) {
            pub.lastRefreshAtTicks = cp->lastRefreshAtTicks;
            pub.lastRefreshMethod = cp->lastRefreshMethod;
            pub.fullIndexedAtTicks = cp->fullIndexedAtTicks;
        }
        const auto snap = evaluateIndexSnapshot(pub, now);
        const auto gate = evaluateIndexAgeGate(snap, request.maxIndexAgeSeconds);
        if (gate.result == IndexAgeGateResult::TooOld) {
            auto report = failReport(request, "index_too_old",
                                     ReclaimPlanSource::PersistentIndex);
            report.snapshot = snap;
            report.ageDecision = gate;
            return report;
        }
        if (gate.result == IndexAgeGateResult::FreshnessUnknown) {
            auto report = failReport(request, "index_freshness_unknown",
                                     ReclaimPlanSource::PersistentIndex);
            report.snapshot = snap;
            report.ageDecision = gate;
            return report;
        }

        ReclaimPlanReport report;
        report.root = request.root;
        report.sourceUsed = ReclaimPlanSource::PersistentIndex;
        report.targetFreeBytes = request.targetFreeBytes;
        report.physicalAccounting = true;
        report.snapshot = snap;
        report.ageDecision = gate;

        const auto ctx = probeProviderLocations(stop);
        report.providerProbeDetail = ctx.probeDetail;
        if (stop.stop_requested()) {
            report.ok = false;
            report.error = "cancelled";
            report.state = "cancelled";
            return report;
        }

        struct DirRow {
            std::int64_t id = 0;
            std::int64_t parentId = 0;
            bool hasParent = false;
            std::wstring path;
            std::wstring name;
            ByteSize logical = 0;
            Classification classification{};
            LocationSafety safety = LocationSafety::Unknown;
            Reclaimability reclaimability = Reclaimability::Unknown;
            CandidateStrength strength = CandidateStrength::None;
            StorageIdentity identity{};
            std::optional<ByteSize> allocated;
            bool allocationKnown = false;
            HardLinkCoverage coverage = HardLinkCoverage::Unknown;
            bool sparse = false;
            bool compressed = false;
        };

        IdentityIndex idx;
        std::vector<DirRow> dirs;
        std::unordered_map<std::int64_t, std::vector<std::wstring>> childNames;
        {
            SqliteStmt stmt(
                store.db(),
                "SELECT id, parent_id, kind, name, path, size_bytes, "
                "recursive_size, classification, confidence, rule_id, "
                "location_safety, reclaimability, candidate_strength, "
                "allocated_bytes, allocation_known, hard_link_count, "
                "observed_link_count, sparse, compressed, volume_serial, "
                "file_id, hard_link_coverage FROM entries;");
            while (stmt.step()) {
                if (stop.stop_requested()) {
                    report.ok = false;
                    report.error = "cancelled";
                    report.state = "cancelled";
                    return report;
                }
                const std::int64_t id = stmt.columnInt64(0);
                const bool isDir = stmt.columnInt64(2) == 1;
                const std::wstring name = stmt.columnText16(3);
                const std::wstring path = stmt.columnText16(4);
                if (stmt.columnType(1) != 5) {
                    childNames[stmt.columnInt64(1)].push_back(name);
                }
                if (isDir) {
                    DirRow row;
                    row.id = id;
                    if (stmt.columnType(1) != 5) {
                        row.parentId = stmt.columnInt64(1);
                        row.hasParent = true;
                    }
                    row.path = path;
                    row.name = name;
                    row.logical = static_cast<ByteSize>(stmt.columnInt64(6));
                    row.classification.category =
                        parseStorageCategory(stmt.columnText(7));
                    row.classification.confidence =
                        parseClassConfidence(stmt.columnText(8));
                    row.classification.ruleId = stmt.columnText(9);
                    row.safety = parseSafetyStored(stmt.columnText(10));
                    row.reclaimability = parseReclaimStored(stmt.columnText(11));
                    row.strength = parseStrengthStored(stmt.columnText(12));
                    if (stmt.columnType(13) != 5) {
                        row.allocated = static_cast<ByteSize>(stmt.columnInt64(13));
                    }
                    row.allocationKnown = stmt.columnInt64(14) != 0;
                    row.sparse = stmt.columnInt64(17) != 0;
                    row.compressed = stmt.columnInt64(18) != 0;
                    row.identity.volumeSerial =
                        static_cast<std::uint64_t>(stmt.columnInt64(19));
                    row.identity.fileId =
                        static_cast<std::uint64_t>(stmt.columnInt64(20));
                    row.coverage = parseHardLinkCoverage(stmt.columnText(21));
                    dirs.push_back(std::move(row));
                } else {
                    IdentSample sample;
                    sample.path = path;
                    sample.identity.volumeSerial =
                        static_cast<std::uint64_t>(stmt.columnInt64(19));
                    sample.identity.fileId =
                        static_cast<std::uint64_t>(stmt.columnInt64(20));
                    if (stmt.columnType(13) != 5) {
                        sample.allocatedBytes =
                            static_cast<ByteSize>(stmt.columnInt64(13));
                    }
                    sample.allocationKnown = stmt.columnInt64(14) != 0;
                    sample.filesystemLinks =
                        static_cast<std::uint32_t>(stmt.columnInt64(15));
                    sample.sparse = stmt.columnInt64(17) != 0;
                    sample.compressed = stmt.columnInt64(18) != 0;
                    addFileSample(idx, std::move(sample));
                }
            }
        }

        std::wstring rootPath = meta->rootPath;
        std::vector<ReclaimCandidateEvidence> candidates;
        for (const auto& dir : dirs) {
            if (pathIsUnderNormalized(rootPath, dir.path) &&
                pathIsUnderNormalized(dir.path, rootPath)) {
                continue;
            }
            ReclaimCandidateEvidence cand;
            cand.path = dir.path;
            cand.kind = ItemKind::Directory;
            cand.identity = dir.identity;
            cand.classification = dir.classification;
            cand.safety = dir.safety;
            cand.reclaimability = dir.reclaimability;
            cand.strength = dir.strength;
            cand.size.logicalBytes = dir.logical;
            cand.size.allocatedBytes = dir.allocated;
            cand.size.allocationKnown = dir.allocationKnown;
            cand.size.hardLinkCoverage = dir.coverage;
            cand.size.sparse = dir.sparse;
            cand.size.compressed = dir.compressed;
            cand.snapshotBased = true;
            cand.evidenceTimeTicks = meta->indexedAtTicks;
            cand.evidenceTimeIso = meta->indexedAtIso;
            const auto* children = childNames.count(dir.id)
                                       ? &childNames[dir.id]
                                       : nullptr;
            const auto* siblings =
                dir.hasParent && childNames.count(dir.parentId)
                    ? &childNames[dir.parentId]
                    : nullptr;
            finishCandidate(cand, idx, ctx,
                            children ? children->data() : nullptr,
                            children ? children->size() : 0,
                            siblings ? siblings->data() : nullptr,
                            siblings ? siblings->size() : 0);
            if (shouldKeepDirectory(cand, dir.logical)) {
                candidates.push_back(std::move(cand));
            }
        }

        {
            SqliteStmt files(
                store.db(),
                "SELECT name, path, size_bytes, classification, confidence, "
                "rule_id, location_safety, reclaimability, candidate_strength, "
                "allocated_bytes, allocation_known, hard_link_count, "
                "observed_link_count, sparse, compressed, volume_serial, file_id "
                "FROM entries WHERE kind = 0;");
            while (files.step()) {
                const ByteSize logical =
                    static_cast<ByteSize>(files.columnInt64(2));
                ReclaimCandidateEvidence cand;
                cand.path = files.columnText16(1);
                cand.kind = ItemKind::File;
                cand.classification.category =
                    parseStorageCategory(files.columnText(3));
                cand.classification.confidence =
                    parseClassConfidence(files.columnText(4));
                cand.classification.ruleId = files.columnText(5);
                cand.safety = parseSafetyStored(files.columnText(6));
                cand.reclaimability = parseReclaimStored(files.columnText(7));
                cand.strength = parseStrengthStored(files.columnText(8));
                cand.size.logicalBytes = logical;
                cand.snapshotBased = true;
                if (!shouldKeepFile(cand, logical)) {
                    continue;
                }
                finishCandidate(cand, idx, ctx, nullptr, 0, nullptr, 0);
                candidates.push_back(std::move(cand));
            }
        }

        assembleReport(report, std::move(candidates), request);
        revalidateBounded(report.actionable, report.reviewOnly, report.selected,
                          stop);
        if (stop.stop_requested()) {
            report.ok = false;
            report.error = "cancelled";
            report.state = "cancelled";
            return report;
        }
        {
            ByteSize selectedSum = 0;
            bool selectedAny = false;
            for (const auto& cand : report.selected) {
                if (cand.hostReclaimBytes.has_value()) {
                    selectedAny = true;
                    (void)addSaturating(selectedSum, *cand.hostReclaimBytes);
                }
            }
            report.selectedHostReclaimBytes =
                selectedAny ? std::optional<ByteSize>(selectedSum)
                            : std::nullopt;
            if (request.targetFreeBytes.has_value()) {
                report.targetMet = selectedSum >= *request.targetFreeBytes;
            }
        }
        report.ok = true;
        report.state = "completed";
        return report;
    } catch (const SqliteError& ex) {
        const std::string msg = ex.what();
        if (msg.find("not found") != std::string::npos) {
            return failReport(request, "index_not_found",
                              ReclaimPlanSource::PersistentIndex);
        }
        return failReport(request, "unsupported_schema",
                          ReclaimPlanSource::PersistentIndex);
    }
}

}  // namespace

const char* toString(ReclaimPlanSource source) noexcept
{
    switch (source) {
    case ReclaimPlanSource::Auto:
        return "auto";
    case ReclaimPlanSource::PersistentIndex:
        return "persistent_index";
    case ReclaimPlanSource::LiveScan:
        return "live_scan";
    }
    return "auto";
}

bool parseReclaimPlanSource(std::string_view text, ReclaimPlanSource& out) noexcept
{
    if (text == "auto") {
        out = ReclaimPlanSource::Auto;
        return true;
    }
    if (text == "persistent_index") {
        out = ReclaimPlanSource::PersistentIndex;
        return true;
    }
    if (text == "live_scan") {
        out = ReclaimPlanSource::LiveScan;
        return true;
    }
    return false;
}

std::string ReclaimPlanReport::toJson() const
{
    std::ostringstream os;
    os << '{'
       << "\"schema_version\":" << kSchemaVersion << ','
       << "\"command\":\"reclaim-plan\","
       << "\"ok\":" << jsonBool(ok) << ','
       << "\"state\":" << jsonString(state) << ','
       << "\"source\":" << jsonString(toString(sourceUsed)) << ','
       << "\"path\":" << jsonString(root) << ','
       << "\"planning_only\":" << jsonBool(true) << ','
       << "\"read_only\":" << jsonBool(true) << ','
       << "\"filesystem_mutation\":" << jsonBool(false) << ','
       << "\"execution_supported\":" << jsonBool(false) << ','
       << "\"physical_accounting\":" << jsonBool(physicalAccounting) << ','
       << "\"hard_link_coverage\":" << jsonString(toString(overallCoverage));
    appendOptionalBytes(os, "target_free_bytes", targetFreeBytes, true);
    os << ",\"target_met\":" << jsonBool(targetMet);
    if (!error.empty()) {
        os << ",\"error\":" << jsonString(error);
    }
    if (!providerProbeDetail.empty()) {
        os << ",\"provider_probe\":" << jsonString(providerProbeDetail);
    }
    if (sourceUsed == ReclaimPlanSource::PersistentIndex) {
        os << ",\"freshness\":"
           << indexFreshnessJsonObject(snapshot, ageDecision);
    }
    os << ",\"summary\":{"
       << "\"actionable_count\":" << jsonUInt(actionable.size()) << ','
       << "\"review_only_count\":" << jsonUInt(reviewOnly.size()) << ','
       << "\"selected_count\":" << jsonUInt(selected.size());
    appendOptionalBytes(os, "exact_host_reclaim_bytes", exactHostReclaimBytes,
                        true);
    appendOptionalBytes(os, "actionable_host_reclaim_bytes",
                        actionableHostReclaimBytes, true);
    appendOptionalBytes(os, "selected_host_reclaim_bytes",
                        selectedHostReclaimBytes, true);
    os << "},";
    appendCandidateArray(os, "actionable", actionable);
    os << ',';
    appendCandidateArray(os, "review_only", reviewOnly);
    os << ',';
    appendCandidateArray(os, "selected", selected);
    os << "}\n";
    return os.str();
}

ReclaimPlanReport buildReclaimPlan(const ReclaimPlanRequest& request,
                                   std::stop_token stop)
{
    if (request.root.empty()) {
        return failReport(request, "inaccessible_root", request.source);
    }
    const DWORD attr = ::GetFileAttributesW(request.root.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES ||
        (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return failReport(request, "inaccessible_root", request.source);
    }

    if (request.source == ReclaimPlanSource::LiveScan) {
        return buildFromLive(request, stop);
    }
    if (request.source == ReclaimPlanSource::PersistentIndex) {
        return buildFromIndex(request, stop);
    }

    const auto loc = locateIndex(request.root);
    if (indexDatabaseExists(loc)) {
        auto indexed = buildFromIndex(request, stop);
        if (indexed.ok) {
            return indexed;
        }
        if (indexed.error == "index_too_old" ||
            indexed.error == "index_freshness_unknown" ||
            indexed.error == "cancelled") {
            return indexed;
        }
        // Missing physical accounting or schema — fall back to one live scan.
    }
    return buildFromLive(request, stop);
}

}  // namespace spacelens
