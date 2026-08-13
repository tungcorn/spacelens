#include "core/CleanupPlan.hpp"

#include "core/Json.hpp"
#include "core/SizeFormatter.hpp"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <ctime>
#include <limits>
#include <sstream>
#include <utility>

namespace spacelens {
namespace {

constexpr ByteSize kMaxBytes = std::numeric_limits<ByteSize>::max();

struct SaturatingTotal {
    ByteSize value = 0;
    bool saturated = false;
};

SaturatingTotal addSaturating(SaturatingTotal total, ByteSize value)
{
    if (kMaxBytes - total.value < value) {
        total.value = kMaxBytes;
        total.saturated = true;
    } else {
        total.value += value;
    }
    return total;
}

std::wstring normalizeSeparators(std::wstring_view value)
{
    std::wstring out(value);
    for (wchar_t& ch : out) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    while (out.size() > 1 && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

std::wstring normalizeForComparison(std::wstring_view value)
{
    auto out = normalizeSeparators(value);
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return out;
}

std::string isoFromFileTime(FileTimeTicks ticks)
{
    if (ticks == 0) {
        return {};
    }
    constexpr std::uint64_t kEpochDifference = 11'644'473'600ULL;
    const std::uint64_t seconds = ticks / kFileTimeTicksPerSecond;
    if (seconds < kEpochDifference) {
        return {};
    }
    const std::time_t unixTime = static_cast<std::time_t>(seconds - kEpochDifference);
    std::tm tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &unixTime) != 0) {
        return {};
    }
#else
    if (gmtime_r(&unixTime, &tm) == nullptr) {
        return {};
    }
#endif
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return buffer;
}

std::string candidateSource(const CleanupCandidate& candidate)
{
    return candidate.source.empty() ? "unknown" : candidate.source;
}

LocationSafety candidateSafety(const CleanupCandidate& candidate)
{
    return candidate.capturedSafety;
}

Reclaimability candidateReclaimability(const CleanupCandidate& candidate)
{
    return candidate.capturedReclaimability;
}

CandidateStrength candidateStrength(const CleanupCandidate& candidate)
{
    return candidate.capturedCandidateStrength;
}

ByteSize capturedBytesFor(const CleanupCandidate& candidate)
{
    const auto evidence = objectEvidenceOf(candidate);
    const auto aggregate = historicalAggregateOf(candidate);
    if (candidate.kind != ItemKind::File && aggregate.available) {
        return aggregate.recursiveLogicalSize;
    }
    return evidence.logicalSize;
}

std::optional<ByteSize> currentBytesFor(const CleanupPlanItem& item)
{
    if (!item.currentEvidence.available || !item.currentEvidence.exists) {
        return std::nullopt;
    }
    if (item.kind != ItemKind::File &&
        item.currentEvidence.directoryAggregate.available) {
        return item.currentEvidence.directoryAggregate.recursiveLogicalSize;
    }
    if (item.currentEvidence.objectEvidence.available) {
        return item.currentEvidence.objectEvidence.logicalSize;
    }
    return std::nullopt;
}

bool isNormalDirectory(const CleanupCandidate& candidate)
{
    return candidate.kind == ItemKind::Directory;
}

// The live enumerator does not follow directory reparse points, so a parent
// recursive aggregate does not include a selected reparse or anything under it.
bool pathCrossesSelectedReparse(const std::vector<CleanupPlanItem>& entries,
                                const CleanupPlanItem& ancestor,
                                const CleanupPlanItem& descendant)
{
    for (const auto& item : entries) {
        if (item.kind != ItemKind::ReparseDirectory) {
            continue;
        }
        if (isStrictPathAncestor(ancestor.path, item.path) &&
            isPathAncestorOrEqual(item.path, descendant.path)) {
            return true;
        }
    }
    return false;
}

bool sameStrongIdentity(const CleanupCandidate& a, const CleanupCandidate& b)
{
    const auto ia = identityOf(a);
    const auto ib = identityOf(b);
    return isStrongIdentity(ia) && isStrongIdentity(ib) &&
           identitiesEqual(ia, ib);
}

bool differentStrongIdentity(const CleanupCandidate& a,
                             const CleanupCandidate& b)
{
    const auto ia = identityOf(a);
    const auto ib = identityOf(b);
    return isStrongIdentity(ia) && isStrongIdentity(ib) &&
           !identitiesEqual(ia, ib);
}

bool cleanupReviewConflict(const CleanupCandidate& a,
                           const CleanupCandidate& b)
{
    if (sameStrongIdentity(a, b) && a.kind != b.kind) {
        return true;
    }
    return normalizeCleanupPath(a.path) == normalizeCleanupPath(b.path) &&
           a.kind == b.kind &&
           (differentStrongIdentity(a, b) ||
            isStrongIdentity(identityOf(a)) != isStrongIdentity(identityOf(b)));
}

bool conservativeFallbackDuplicate(const CleanupCandidate& a,
                                    const CleanupCandidate& b)
{
    return normalizeCleanupPath(a.path) == normalizeCleanupPath(b.path) &&
           a.kind == b.kind;
}

void addReason(CleanupPlanItem& item, std::string reason)
{
    if (std::find(item.planningReasons.begin(), item.planningReasons.end(),
                  reason) == item.planningReasons.end()) {
        item.planningReasons.push_back(std::move(reason));
    }
}

void sortReasons(std::vector<std::string>& reasons)
{
    std::sort(reasons.begin(), reasons.end());
}

void sortDiffs(std::vector<CleanupValidationDiff>& diffs)
{
    std::sort(diffs.begin(), diffs.end(), [](const auto& a, const auto& b) {
        if (std::string_view{toString(a.kind)} !=
            std::string_view{toString(b.kind)}) {
            return std::string_view{toString(a.kind)} <
                   std::string_view{toString(b.kind)};
        }
        if (a.captured != b.captured) {
            return a.captured < b.captured;
        }
        return a.current < b.current;
    });
}

std::string optionalSizeJson(const std::optional<ByteSize>& value)
{
    return value ? jsonUInt(*value) : "null";
}

std::string identityJson(const CleanupIdentity& identity)
{
    std::ostringstream os;
    os << "{\"available\":" << jsonBool(isIdentityAvailable(identity))
       << ",\"source\":" << jsonString(toString(identity.source))
       << ",\"strength\":" << jsonString(toString(identity.source))
       << ",\"volume_serial\":" << jsonUInt(identity.volumeSerial)
       << ",\"file_index_64\":" << jsonUInt(identity.fileIndex64)
       << ",\"id_128\":";
    std::string id;
    id.reserve(identity.fileId128.size() * 2);
    for (const auto byte : identity.fileId128) {
        char buffer[3];
        std::snprintf(buffer, sizeof(buffer), "%02x", byte);
        id += buffer;
    }
    os << jsonString(id) << "}";
    return os.str();
}

std::string objectEvidenceJson(const CleanupObjectEvidence& evidence)
{
    std::ostringstream os;
    os << "{\"available\":" << jsonBool(evidence.available)
       << ",\"identity\":" << identityJson(evidence.identity)
       << ",\"kind\":" << jsonString(toString(evidence.kind))
       << ",\"scope\":" << jsonString(toString(evidence.sizeScope))
       << ",\"logical_size_bytes\":"
       << jsonUInt(evidence.logicalSize)
       << ",\"last_write_time_ticks\":"
       << jsonUInt(evidence.lastWriteTime)
       << ",\"last_access_time_ticks\":"
       << jsonUInt(evidence.lastAccessTime)
       << ",\"attributes\":" << jsonUInt(evidence.attributes) << "}";
    return os.str();
}

std::string aggregateJson(const CleanupDirectoryAggregateEvidence& evidence)
{
    std::ostringstream os;
    os << "{\"available\":" << jsonBool(evidence.available)
       << ",\"revalidated\":" << jsonBool(evidence.revalidated)
       << ",\"recursive_logical_size_bytes\":"
       << jsonUInt(evidence.recursiveLogicalSize)
       << ",\"newest_descendant_write_ticks\":"
       << jsonUInt(evidence.newestDescendantWrite) << "}";
    return os.str();
}

std::string currentEvidenceJson(const CleanupCurrentEvidence& evidence)
{
    std::ostringstream os;
    os << "{\"available\":" << jsonBool(evidence.available)
       << ",\"exists\":" << jsonBool(evidence.exists)
       << ",\"safety\":" << jsonString(toString(evidence.safety))
       << ",\"object\":" << objectEvidenceJson(evidence.objectEvidence)
       << ",\"directory_aggregate\":"
       << aggregateJson(evidence.directoryAggregate) << "}";
    return os.str();
}

std::string classificationJson(const Classification& classification)
{
    std::ostringstream os;
    os << "{\"category\":"
       << jsonString(toString(classification.category))
       << ",\"confidence\":"
       << jsonString(toString(classification.confidence))
       << ",\"rule_id\":" << jsonString(classification.ruleId)
       << ",\"reason\":" << jsonString(classification.reason) << "}";
    return os.str();
}

std::string reasonsJson(const std::vector<std::string>& reasons)
{
    std::ostringstream os;
    os << '[';
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        if (i != 0) {
            os << ',';
        }
        os << jsonString(reasons[i]);
    }
    os << ']';
    return os.str();
}

std::string diffsJson(const std::vector<CleanupValidationDiff>& diffs)
{
    std::ostringstream os;
    os << '[';
    for (std::size_t i = 0; i < diffs.size(); ++i) {
        if (i != 0) {
            os << ',';
        }
        os << "{\"kind\":" << jsonString(toString(diffs[i].kind))
           << ",\"captured\":" << jsonString(diffs[i].captured)
           << ",\"current\":" << jsonString(diffs[i].current) << "}";
    }
    os << ']';
    return os.str();
}

std::string itemJson(const CleanupPlanItem& item,
                     std::wstring_view profilePath)
{
    const auto path = redactUserProfilePath(item.path, profilePath);
    const auto sourceRoot = redactUserProfilePath(item.candidate.sourceRoot,
                                                  profilePath);
    const auto identity = identityOf(item.candidate);
    const auto reasons = validationReasonNames(item.validation.reasons);
    std::ostringstream os;
    os << "{\"id\":" << jsonUInt(item.id)
       << ",\"path\":" << jsonString(path)
       << ",\"kind\":" << jsonString(toString(item.kind))
       << ",\"included\":" << jsonBool(item.included)
       << ",\"suppressed\":" << jsonBool(item.suppressed)
       << ",\"conflict\":" << jsonBool(item.conflict)
       << ",\"suppressed_by_id\":" << jsonUInt(item.suppressedById)
       << ",\"captured_size_bytes\":" << jsonUInt(item.capturedLogicalBytes)
       << ",\"current_size_bytes\":"
       << optionalSizeJson(item.currentLogicalBytes)
       << ",\"comparable_current_size_bytes\":"
       << optionalSizeJson(item.comparableCurrentLogicalBytes)
       << ",\"captured_evidence\":{\"object\":"
       << objectEvidenceJson(item.capturedEvidence)
       << ",\"directory_aggregate\":"
       << aggregateJson(item.capturedDirectoryAggregate) << "}"
       << ",\"current_evidence\":"
       << currentEvidenceJson(item.currentEvidence)
       << ",\"identity\":" << identityJson(identity)
       << ",\"classification\":" << classificationJson(item.classification)
       << ",\"safety\":" << jsonString(toString(item.safety))
       << ",\"reclaimability\":"
       << jsonString(toString(item.reclaimability))
       << ",\"candidate_strength\":"
       << jsonString(toString(item.candidateStrength))
       << ",\"state\":" << jsonString(toString(item.validation.state))
       << ",\"reasons\":" << reasonsJson(reasons)
       << ",\"planning_reasons\":" << reasonsJson(item.planningReasons)
       << ",\"diffs\":" << diffsJson(item.validation.diffs)
       << ",\"source\":" << jsonString(item.source)
       << ",\"source_root\":" << jsonString(sourceRoot)
       << ",\"added_at_ticks\":"
       << jsonUInt(item.candidate.addedAt)
       << ",\"added_at\":"
       << jsonString(isoFromFileTime(item.candidate.addedAt))
       << "}";
    return os.str();
}

}  // namespace

std::wstring redactUserProfilePath(std::wstring_view path,
                                   std::wstring_view userProfilePath)
{
    if (path.empty() || userProfilePath.empty()) {
        return std::wstring(path);
    }
    const std::wstring normalizedPath = normalizeForComparison(path);
    const std::wstring normalizedProfile = normalizeForComparison(userProfilePath);
    if (normalizedProfile.empty() || normalizedPath.size() < normalizedProfile.size() ||
        normalizedPath.compare(0, normalizedProfile.size(), normalizedProfile) != 0) {
        return std::wstring(path);
    }
    if (normalizedPath.size() != normalizedProfile.size() &&
        normalizedPath[normalizedProfile.size()] != L'\\') {
        return std::wstring(path);
    }
    const std::wstring outputPath = normalizeSeparators(path);
    std::wstring suffix = outputPath.substr(normalizedProfile.size());
    if (!suffix.empty() && suffix.front() != L'\\') {
        return std::wstring(path);
    }
    return std::wstring(L"%USERPROFILE%") + suffix;
}

std::string cleanupPlanGeneratedAt(const CleanupPlanOptions& options)
{
    if (!options.generatedAt.empty()) {
        return options.generatedAt;
    }
    return "1970-01-01T00:00:00Z";
}

CleanupPlan buildCleanupPlan(const CleanupReview& review,
                             const CleanupPlanOptions& options)
{
    CleanupPlan plan;
    plan.generatedAt = cleanupPlanGeneratedAt(options);
    plan.summary.selectedCount = review.size();

    std::vector<CleanupPlanItem> entries;
    entries.reserve(review.items().size());
    for (const auto& candidate : review.items()) {
        CleanupPlanItem item;
        item.id = candidate.id;
        item.path = candidate.path;
        item.kind = candidate.kind;
        item.candidate = candidate;
        item.capturedEvidence = objectEvidenceOf(candidate);
        item.capturedDirectoryAggregate = historicalAggregateOf(candidate);
        item.currentEvidence = candidate.currentEvidence;
        item.capturedLogicalBytes = capturedBytesFor(candidate);
        item.classification = candidate.classification;
        item.safety = candidateSafety(candidate);
        item.reclaimability = candidateReclaimability(candidate);
        item.candidateStrength = candidateStrength(candidate);
        item.source = candidateSource(candidate);
        item.validation = validateCleanupCandidate(candidate, item.currentEvidence);
        item.currentLogicalBytes = currentBytesFor(item);
        if (item.currentEvidence.available && item.currentEvidence.exists &&
            (item.kind == ItemKind::File ||
             (item.currentEvidence.directoryAggregate.available &&
              item.currentEvidence.directoryAggregate.revalidated))) {
            item.comparableCurrentLogicalBytes = item.currentLogicalBytes;
        }
        if (!isIdentityAvailable(identityOf(candidate))) {
            addReason(item, "identity_unavailable");
            plan.summary.estimated = true;
        }
        if (item.kind != ItemKind::File &&
            (!item.capturedDirectoryAggregate.available ||
             !item.currentEvidence.directoryAggregate.revalidated)) {
            addReason(item, "directory_recursive_evidence_estimated");
            plan.summary.estimated = true;
        }
        if (!item.currentEvidence.available) {
            addReason(item, "current_evidence_unavailable");
            plan.summary.estimated = true;
        }
        entries.push_back(std::move(item));
    }

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        const auto pathA = normalizeCleanupPath(a.path);
        const auto pathB = normalizeCleanupPath(b.path);
        if (pathA != pathB) {
            return pathA < pathB;
        }
        if (a.kind != b.kind) {
            return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        }
        return a.id < b.id;
    });

    // CleanupReview does not persist conflict links, so recompute its conflict
    // predicates before duplicate marking. The higher-id item is the candidate
    // accepted as the later conflict by CleanupReview.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        for (std::size_t j = i + 1; j < entries.size(); ++j) {
            if (!cleanupReviewConflict(entries[i].candidate,
                                       entries[j].candidate)) {
                continue;
            }
            auto& conflict = entries[i].id > entries[j].id ? entries[i]
                                                             : entries[j];
            conflict.conflict = true;
            conflict.included = true;
            addReason(conflict, "identity_conflict");
            plan.conflictIds.push_back(conflict.id);
        }
    }

    // Identity-first duplicate marking. Same-kind strong-identity matches are
    // true duplicates even when both rows also conflict with another kind.
    // Path/type fallback must not swallow IdentityConflicts.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto& current = entries[i];
        for (std::size_t j = 0; j < i; ++j) {
            auto& prior = entries[j];
            if (current.kind != prior.kind) {
                continue;
            }
            const bool sameIdentity =
                sameStrongIdentity(current.candidate, prior.candidate);
            if (sameIdentity ||
                (!current.conflict &&
                 conservativeFallbackDuplicate(current.candidate,
                                               prior.candidate))) {
                current.included = false;
                current.suppressed = true;
                current.suppressedById = prior.id;
                addReason(current, sameIdentity ? "duplicate_strong_identity"
                                                : "duplicate_path_and_type");
                plan.suppressedIds.push_back(current.id);
                break;
            }
        }
    }

    // An included ordinary directory with a captured recursive aggregate covers
    // selected descendants whose contents are inside that no-follow aggregate.
    // Files and reparse directories never cover descendants. A selected reparse
    // and anything under it stay included: the enumerator does not follow
    // directory reparse points, so those bytes are not in the ancestor total.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& ancestor = entries[i];
        if (!ancestor.included || !isNormalDirectory(ancestor.candidate) ||
            !ancestor.capturedDirectoryAggregate.available) {
            continue;
        }
        for (std::size_t j = 0; j < entries.size(); ++j) {
            auto& descendant = entries[j];
            if (i == j || !descendant.included ||
                !isStrictPathAncestor(ancestor.path, descendant.path) ||
                pathCrossesSelectedReparse(entries, ancestor, descendant)) {
                continue;
            }
            descendant.included = false;
            descendant.suppressed = true;
            descendant.suppressedById = ancestor.id;
            addReason(descendant,
                      "normal_directory_recursive_aggregate_covers_descendant");
            if (std::find(plan.suppressedIds.begin(), plan.suppressedIds.end(),
                          descendant.id) == plan.suppressedIds.end()) {
                plan.suppressedIds.push_back(descendant.id);
            }
        }
    }

    std::sort(plan.suppressedIds.begin(), plan.suppressedIds.end());
    std::sort(plan.conflictIds.begin(), plan.conflictIds.end());
    plan.suppressedIds.erase(
        std::unique(plan.suppressedIds.begin(), plan.suppressedIds.end()),
        plan.suppressedIds.end());
    plan.conflictIds.erase(
        std::unique(plan.conflictIds.begin(), plan.conflictIds.end()),
        plan.conflictIds.end());

    SaturatingTotal raw;
    SaturatingTotal unique;
    for (const auto& candidate : review.items()) {
        raw = addSaturating(raw, capturedBytesFor(candidate));
    }
    for (auto& item : entries) {
        sortReasons(item.planningReasons);
        sortDiffs(item.validation.diffs);
        if (item.included) {
            unique = addSaturating(unique, item.capturedLogicalBytes);
        }
    }
    plan.items = std::move(entries);
    plan.summary.itemCount = plan.items.size();
    plan.summary.includedCount = static_cast<std::size_t>(std::count_if(
        plan.items.begin(), plan.items.end(),
        [](const auto& item) { return item.included; }));
    plan.summary.suppressedCount = plan.suppressedIds.size();
    plan.summary.conflictCount = plan.conflictIds.size();
    plan.summary.rawLogicalBytes = raw.value;
    plan.summary.uniqueLogicalBytes = unique.value;
    plan.summary.rawSumSaturated = raw.saturated;
    plan.summary.uniqueSumSaturated = unique.saturated;
    if (raw.saturated || unique.saturated) {
        plan.summary.estimated = true;
        plan.summary.estimatedReasons.push_back("saturating_sum");
    }
    if (!plan.suppressedIds.empty()) {
        plan.summary.estimated = true;
        plan.summary.estimatedReasons.push_back("overlap_suppressed");
    }
    if (!plan.conflictIds.empty()) {
        plan.summary.estimated = true;
        plan.summary.estimatedReasons.push_back("identity_conflict");
    }
    for (const auto& item : plan.items) {
        for (const auto& reason : item.planningReasons) {
            if (reason == "identity_unavailable" ||
                reason == "current_evidence_unavailable" ||
                reason == "directory_recursive_evidence_estimated") {
                plan.summary.estimatedReasons.push_back(reason);
            }
        }
    }
    std::sort(plan.summary.estimatedReasons.begin(),
              plan.summary.estimatedReasons.end());
    plan.summary.estimatedReasons.erase(
        std::unique(plan.summary.estimatedReasons.begin(),
                    plan.summary.estimatedReasons.end()),
        plan.summary.estimatedReasons.end());
    return plan;
}

std::string CleanupPlan::toText() const
{
    return toText(CleanupPlanOptions{});
}

std::string CleanupPlan::toText(const CleanupPlanOptions& options) const
{
    std::ostringstream os;
    os << "SpaceLens Cleanup Review / Plan (planning only — no deletion or move "
          "authorization)\n";
    os << "Selected: " << summary.selectedCount << " items\n";
    os << "Unique selected logical size: "
       << SizeFormatter::format(summary.uniqueLogicalBytes) << "\n";
    os << "Raw selected logical size: "
       << SizeFormatter::format(summary.rawLogicalBytes) << "\n";
    os << "Estimated: " << (summary.estimated ? "yes" : "no") << "\n";
    if (!summary.estimatedReasons.empty()) {
        os << "Estimated reasons: ";
        for (std::size_t i = 0; i < summary.estimatedReasons.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            os << summary.estimatedReasons[i];
        }
        os << "\n";
    }
    os << "\n";

    for (const auto& item : items) {
        os << "- [" << toString(item.kind) << "] "
           << utf8FromWide(redactUserProfilePath(item.path,
                                                   options.userProfilePath))
           << "\n";
        os << "  captured size: "
           << SizeFormatter::format(item.capturedLogicalBytes) << "\n";
        os << "  current comparable size: ";
        if (item.comparableCurrentLogicalBytes) {
            os << SizeFormatter::format(*item.comparableCurrentLogicalBytes);
        } else {
            os << "unknown";
        }
        os << "\n";
        os << "  classification: " << toString(item.classification.category)
           << " (" << toString(item.classification.confidence) << ")\n";
        os << "  safety: " << toString(item.safety)
           << "  reclaimability: " << toString(item.reclaimability)
           << "  candidate strength: " << toString(item.candidateStrength) << "\n";
        os << "  state: " << toString(item.validation.state) << "\n";
        os << "  reasons: ";
        const auto reasons = validationReasonNames(item.validation.reasons);
        for (std::size_t i = 0; i < reasons.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            os << reasons[i];
        }
        if (!item.planningReasons.empty()) {
            os << "; planning=";
            for (std::size_t i = 0; i < item.planningReasons.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                os << item.planningReasons[i];
            }
        }
        os << "\n";
        if (!item.validation.diffs.empty()) {
            os << "  diffs: ";
            for (std::size_t i = 0; i < item.validation.diffs.size(); ++i) {
                const auto& diff = item.validation.diffs[i];
                if (i != 0) {
                    os << "; ";
                }
                os << toString(diff.kind) << " (captured=" << diff.captured
                   << ", current=" << diff.current << ')';
            }
            os << "\n";
        }
        os << "  source: " << item.source;
        if (!item.candidate.sourceRoot.empty()) {
            os << " (" << utf8FromWide(redactUserProfilePath(
                item.candidate.sourceRoot, options.userProfilePath))
               << ")";
        }
        os << "\n";
        if (item.suppressed) {
            os << "  planning: suppressed by item " << item.suppressedById
               << "\n";
        }
        if (item.conflict) {
            os << "  planning: identity conflict retained separately\n";
        }
        os << "\n";
    }
    os << "Note: This is a planning-only report. It is not authorization to "
          "delete, move, or mutate files.\n";
    return os.str();
}

std::string CleanupPlan::toJson(const CleanupPlanOptions& options) const
{
    const std::wstring& profile = options.userProfilePath;
    std::ostringstream os;
    os << "{\"plan_schema_version\":" << planSchemaVersion
       << ",\"generated_at\":" << jsonString(generatedAt)
       << ",\"planning_only\":true"
       << ",\"read_only\":true"
       << ",\"filesystem_mutation\":false"
       << ",\"summary\":{\"selected_count\":"
       << jsonUInt(summary.selectedCount)
       << ",\"item_count\":" << jsonUInt(summary.itemCount)
       << ",\"included_count\":" << jsonUInt(summary.includedCount)
       << ",\"suppressed_count\":" << jsonUInt(summary.suppressedCount)
       << ",\"conflict_count\":" << jsonUInt(summary.conflictCount)
       << ",\"raw_logical_bytes\":" << jsonUInt(summary.rawLogicalBytes)
       << ",\"unique_logical_bytes\":"
       << jsonUInt(summary.uniqueLogicalBytes)
       << ",\"raw_sum_saturated\":"
       << jsonBool(summary.rawSumSaturated)
       << ",\"unique_sum_saturated\":"
       << jsonBool(summary.uniqueSumSaturated)
       << ",\"estimated\":" << jsonBool(summary.estimated)
       << ",\"estimated_reasons\":"
       << reasonsJson(summary.estimatedReasons) << "}"
       << ",\"suppressed_ids\":[";
    for (std::size_t i = 0; i < suppressedIds.size(); ++i) {
        if (i != 0) {
            os << ',';
        }
        os << jsonUInt(suppressedIds[i]);
    }
    os << "],\"conflict_ids\":[";
    for (std::size_t i = 0; i < conflictIds.size(); ++i) {
        if (i != 0) {
            os << ',';
        }
        os << jsonUInt(conflictIds[i]);
    }
    os << "],\"items\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            os << ',';
        }
        os << itemJson(items[i], profile);
    }
    os << "]}";
    return os.str();
}

std::string cleanupPlanText(const CleanupReview& review,
                            const CleanupPlanOptions& options)
{
    return buildCleanupPlan(review, options).toText(options);
}

std::string cleanupPlanJson(const CleanupReview& review,
                            const CleanupPlanOptions& options)
{
    return buildCleanupPlan(review, options).toJson(options);
}

}  // namespace spacelens
