#include "core/StorageIntelligence.hpp"

#include "core/FileTime.hpp"
#include "core/Json.hpp"
#include "core/Query.hpp"
#include "core/SafetyPolicy.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace spacelens {
namespace {

std::wstring normalizePathKey(std::wstring path)
{
    for (wchar_t& ch : path) {
        if (ch == L'/') {
            ch = L'\\';
        } else if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    while (path.size() > 3 && path.back() == L'\\') {
        path.pop_back();
    }
    // Drive root "d:\" is a component prefix of "d:\users\...". Keeping the
    // trailing slash makes isComponentPrefix look for "d:\\" and match nothing.
    if (path.size() == 3 && path[1] == L':' && path[2] == L'\\') {
        path.pop_back();
    }
    return path;
}

std::size_t pathDepth(const std::wstring& key)
{
    std::size_t depth = 0;
    for (wchar_t ch : key) {
        if (ch == L'\\') {
            ++depth;
        }
    }
    return depth;
}

bool isComponentPrefix(const std::wstring& ancestor, const std::wstring& descendant)
{
    if (ancestor.empty() || descendant.empty() || ancestor == descendant) {
        return false;
    }
    if (descendant.size() <= ancestor.size()) {
        return false;
    }
    if (descendant.compare(0, ancestor.size(), ancestor) != 0) {
        return false;
    }
    return descendant[ancestor.size()] == L'\\';
}

Classification classifyDirFromTree(const DirectoryTree& tree, DirIndex idx)
{
    return classifyDirectoryFromTree(tree, idx);
}

int strengthRank(const std::string& value)
{
    if (value == "Strong") {
        return 3;
    }
    if (value == "Moderate") {
        return 2;
    }
    if (value == "ReviewOnly") {
        return 1;
    }
    return 0;
}

int confidenceRank(const std::string& value)
{
    if (value == "High") {
        return 3;
    }
    if (value == "Medium") {
        return 2;
    }
    if (value == "Low") {
        return 1;
    }
    return 0;
}

const char* ecosystemFromRuleId(std::string_view ruleId)
{
    if (ruleId == "node-modules") {
        return "node";
    }
    if (ruleId == "cmake-build-dir" || ruleId == "cmake-build-partial" ||
        ruleId == "cmake-build-prefix") {
        return "cmake";
    }
    if (ruleId == "rust-target-dir") {
        return "rust";
    }
    if (ruleId == "dotnet-bin-obj" || ruleId == "msvc-config-dir") {
        return "dotnet";
    }
    if (ruleId == "python-venv" || ruleId == "python-cache" ||
        ruleId == "python-venv-name") {
        return "python";
    }
    if (ruleId == "nuget-packages-path" || ruleId == "nuget-localappdata") {
        return "nuget";
    }
    if (ruleId == "package-cache-name") {
        return "";
    }
    if (ruleId == "git-metadata") {
        return "git";
    }
    return "";
}

ReclaimCandidate analyzeDirectory(const DirectoryTree& tree, DirIndex idx,
                                  FileTimeTicks nowTicks)
{
    const auto& node = tree.dir(idx);
    const std::wstring path = tree.pathOfDirectory(idx);
    return analyzeItem(path, ItemKind::Directory, node.recursiveSize,
                       node.newestDescendantWrite, classifyDirFromTree(tree, idx),
                       classifyLocation(path), nowTicks, 0);
}

ReclaimCandidate analyzeFileEntry(const DirectoryTree& tree, FileIndex idx,
                                  FileTimeTicks nowTicks)
{
    const auto& file = tree.file(idx);
    const std::wstring path = tree.pathOfFile(idx);
    return analyzeItem(path, ItemKind::File, file.size, file.lastWriteTime,
                       classifyFile(file.name, path), classifyLocation(path),
                       nowTicks, file.lastAccessTime);
}

bool isRegenerable(Reclaimability value)
{
    return value == Reclaimability::LikelyRegenerable ||
           value == Reclaimability::PossiblyRegenerable;
}

bool isOldLargeFile(const ReclaimCandidate& candidate, const OpportunityQuery& query)
{
    if (candidate.kind != ItemKind::File) {
        return false;
    }
    if (candidate.size_bytes < query.minSize) {
        return false;
    }
    if (query.olderThanDays == 0 || candidate.activityWriteTime == 0 ||
        query.nowTicks == 0) {
        return false;
    }
    return isOlderThanDays(candidate.activityWriteTime, query.nowTicks,
                           query.olderThanDays);
}

bool pathIsUnderPrefix(const std::wstring& path, const std::wstring& prefix)
{
    if (prefix.empty()) {
        return true;
    }
    const std::wstring key = normalizePathKey(path);
    const std::wstring pre = normalizePathKey(prefix);
    return key == pre || isComponentPrefix(pre, key);
}

bool includeOpportunity(const ReclaimCandidate& candidate,
                        const OpportunityQuery& query)
{
    if (candidate.strength == CandidateStrength::None) {
        return false;
    }
    if (isMutationDisallowed(candidate.safety) ||
        candidate.safety == LocationSafety::Protected) {
        return false;
    }
    if (query.matchNone) {
        return false;
    }
    if (!pathIsUnderPrefix(candidate.path, query.pathPrefix)) {
        return false;
    }
    if (query.categoryOnly &&
        candidate.classification.category != *query.categoryOnly) {
        return false;
    }
    const bool confidentEnough =
        candidate.classification.confidence == Confidence::High ||
        candidate.classification.confidence == Confidence::Medium;
    if (isRegenerable(candidate.reclaimability) && confidentEnough &&
        candidate.size_bytes >= query.minSize) {
        return true;
    }
    return isOldLargeFile(candidate, query);
}

StorageCategory parseStoredCategory(const std::string& text)
{
    return parseStorageCategory(text);
}

CandidateStrength parseStoredStrength(const std::string& text)
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

Reclaimability parseStoredReclaim(const std::string& text)
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

LocationSafety parseStoredSafety(const std::string& text)
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

ItemKind kindFromHit(const IndexHit& hit)
{
    return hit.kind == IndexEntryKind::Directory ? ItemKind::Directory
                                                 : ItemKind::File;
}

ReclaimCandidate candidateFromHit(const IndexHit& hit, FileTimeTicks nowTicks)
{
    Classification cls;
    cls.category = parseStoredCategory(hit.classification);
    cls.confidence = hit.confidence == "High"     ? Confidence::High
                     : hit.confidence == "Medium" ? Confidence::Medium
                                                  : Confidence::Low;
    cls.ruleId = hit.rule_id;
    const LocationSafety safety = parseStoredSafety(hit.location_safety);
    auto candidate =
        analyzeItem(hit.path, kindFromHit(hit), hit.size_bytes, hit.last_write_ticks,
                    std::move(cls), safety, nowTicks, 0);
    // Prefer persisted index analysis when present so index-backed reports stay
    // consistent with query output. analyzeItem still supplies activity notes.
    if (!hit.reclaimability.empty()) {
        candidate.reclaimability = parseStoredReclaim(hit.reclaimability);
    }
    if (!hit.candidate_strength.empty()) {
        candidate.strength = parseStoredStrength(hit.candidate_strength);
    }
    if (!hit.location_safety.empty()) {
        candidate.safety = safety;
    }
    return candidate;
}

StorageConsumer consumerFromCandidate(const ReclaimCandidate& candidate,
                                      const char* sizeReason)
{
    StorageConsumer out;
    out.path = candidate.path;
    out.objectType = objectTypeName(candidate.kind);
    out.logicalBytes = candidate.size_bytes;
    out.classification = toString(candidate.classification.category);
    out.confidence = toString(candidate.classification.confidence);
    out.reclaimability = toString(candidate.reclaimability);
    out.locationSafety = toString(candidate.safety);
    out.candidateStrength = toString(candidate.strength);
    out.reasonCodes = reasonCodesFor(candidate, false);
    if (sizeReason != nullptr) {
        if (std::find(out.reasonCodes.begin(), out.reasonCodes.end(),
                      sizeReason) == out.reasonCodes.end()) {
            out.reasonCodes.insert(out.reasonCodes.begin(), sizeReason);
        }
    }
    return out;
}

OpportunityItem itemFromCandidate(const ReclaimCandidate& candidate,
                                  bool oldLarge)
{
    OpportunityItem item;
    item.path = candidate.path;
    item.objectType = objectTypeName(candidate.kind);
    item.logicalBytes = candidate.size_bytes;
    item.classification = toString(candidate.classification.category);
    item.confidence = toString(candidate.classification.confidence);
    item.ruleId = candidate.classification.ruleId;
    item.reclaimability = toString(candidate.reclaimability);
    item.candidateStrength = toString(candidate.strength);
    item.locationSafety = toString(candidate.safety);
    if (candidate.activityWriteTime != 0) {
        item.inactiveDays = candidate.inactiveDays;
    }
    item.activityWriteTicks = candidate.activityWriteTime;
    item.reasonCodes = reasonCodesFor(candidate, oldLarge);
    item.explanation = candidate.explanation;
    item.ecosystem = candidate.classification.ecosystem;
    item.marker = candidate.classification.marker;
    if (item.ecosystem.empty()) {
        item.ecosystem = ecosystemFromRuleId(candidate.classification.ruleId);
    }
    return item;
}

void markOverlaps(std::vector<OpportunityItem>& items)
{
    std::vector<std::pair<std::size_t, std::wstring>> dirs;
    dirs.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].objectType == "directory") {
            dirs.push_back({i, normalizePathKey(items[i].path)});
        }
    }
    std::sort(dirs.begin(), dirs.end(),
              [](const auto& a, const auto& b) {
                  const auto da = pathDepth(a.second);
                  const auto db = pathDepth(b.second);
                  if (da != db) {
                      return da < db;
                  }
                  return a.second < b.second;
              });

    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::wstring key = normalizePathKey(items[i].path);
        for (const auto& dir : dirs) {
            if (dir.first == i) {
                continue;
            }
            if (isComponentPrefix(dir.second, key)) {
                items[i].overlapped = true;
                if (std::find(items[i].reasonCodes.begin(), items[i].reasonCodes.end(),
                              reason::kNestedOverlap) == items[i].reasonCodes.end()) {
                    items[i].reasonCodes.push_back(reason::kNestedOverlap);
                }
                break;
            }
        }
    }
}

void sortOpportunities(std::vector<OpportunityItem>& items)
{
    std::sort(items.begin(), items.end(),
              [](const OpportunityItem& a, const OpportunityItem& b) {
                  const int sa = strengthRank(a.candidateStrength);
                  const int sb = strengthRank(b.candidateStrength);
                  if (sa != sb) {
                      return sa > sb;
                  }
                  const int ca = confidenceRank(a.confidence);
                  const int cb = confidenceRank(b.confidence);
                  if (ca != cb) {
                      return ca > cb;
                  }
                  if (a.logicalBytes != b.logicalBytes) {
                      return a.logicalBytes > b.logicalBytes;
                  }
                  return normalizePathKey(a.path) < normalizePathKey(b.path);
              });
}

void assignRanksAndBounds(OpportunityReport& report, std::size_t limit)
{
    sortOpportunities(report.opportunities);
    const std::size_t total = report.opportunities.size();
    report.truncated = limit > 0 && total > limit;
    if (limit > 0 && report.opportunities.size() > limit) {
        report.opportunities.resize(limit);
    }
    for (std::size_t i = 0; i < report.opportunities.size(); ++i) {
        report.opportunities[i].opportunityRank = static_cast<int>(i + 1);
    }
    report.returnedCount = report.opportunities.size();
}

void buildGroupsAndUniqueBytes(OpportunityReport& report)
{
    struct Acc {
        OpportunityGroup group;
    };
    std::vector<Acc> acc;
    auto findOrAdd = [&](const std::string& id,
                         const std::string& classification) -> Acc& {
        for (auto& a : acc) {
            if (a.group.id == id) {
                return a;
            }
        }
        Acc next;
        next.group.id = id;
        next.group.classification = classification;
        acc.push_back(std::move(next));
        return acc.back();
    };

    ByteSize unique = 0;
    bool estimated = false;
    for (const auto& item : report.opportunities) {
        if (!item.overlapped) {
            const ByteSize next = unique + item.logicalBytes;
            if (next < unique) {
                unique = ~ByteSize{0};
                estimated = true;
            } else {
                unique = next;
            }
        }
        std::string groupId = opportunityGroupId(
            parseStorageCategory(item.classification));
        if (groupId.empty() &&
            std::find(item.reasonCodes.begin(), item.reasonCodes.end(),
                      reason::kOldLargeFile) != item.reasonCodes.end()) {
            groupId = "old_large_files";
        }
        if (groupId.empty()) {
            continue;
        }
        Acc& slot = findOrAdd(groupId, groupId == "old_large_files"
                                           ? std::string("mixed")
                                           : item.classification);
        if (!item.overlapped) {
            const ByteSize next = slot.group.logicalBytes + item.logicalBytes;
            if (next < slot.group.logicalBytes) {
                slot.group.logicalBytes = ~ByteSize{0};
                slot.group.estimated = true;
            } else {
                slot.group.logicalBytes = next;
            }
        }
        ++slot.group.itemCount;
        if (strengthRank(item.candidateStrength) >
            strengthRank(slot.group.strongestCandidateStrength)) {
            slot.group.strongestCandidateStrength = item.candidateStrength;
        }
        if (slot.group.reasonCodes.empty()) {
            if (groupId == "developer_dependencies") {
                slot.group.reasonCodes = {reason::kDeveloperDependency,
                                          reason::kLikelyRegenerable};
            } else if (groupId == "generated_outputs") {
                slot.group.reasonCodes = {reason::kGeneratedOutput,
                                          reason::kLikelyRegenerable};
            } else if (groupId == "package_cache") {
                slot.group.reasonCodes = {reason::kPackageCache,
                                          reason::kLikelyRegenerable};
            } else if (groupId == "ide_cache") {
                slot.group.reasonCodes = {reason::kIdeCache,
                                          reason::kLikelyRegenerable};
            } else if (groupId == "temporary_data") {
                slot.group.reasonCodes = {reason::kTemporaryData,
                                          reason::kLikelyRegenerable};
            } else if (groupId == "log_data") {
                slot.group.reasonCodes = {reason::kLogData,
                                          reason::kPossiblyRegenerable};
            } else {
                slot.group.reasonCodes = {reason::kOldLargeFile};
            }
        }
    }

    std::sort(acc.begin(), acc.end(), [](const Acc& a, const Acc& b) {
        if (a.group.logicalBytes != b.group.logicalBytes) {
            return a.group.logicalBytes > b.group.logicalBytes;
        }
        return a.group.id < b.group.id;
    });
    report.groups.clear();
    report.groups.reserve(acc.size());
    for (auto& a : acc) {
        if (a.group.itemCount > 0) {
            report.groups.push_back(std::move(a.group));
        }
    }
    report.uniqueReviewBytes = unique;
    report.uniqueReviewEstimated = estimated;
}

void fillTreeSummary(OpportunityReport& report, const DirectoryTree& tree)
{
    if (tree.empty()) {
        return;
    }
    report.root = tree.pathOfDirectory(tree.root());
    report.logicalBytes = tree.dir(tree.root()).recursiveSize;
    report.files = tree.fileCount();
    report.directories = tree.directoryCount();
}

std::string jsonStringArray(const std::vector<std::string>& values)
{
    std::ostringstream os;
    os << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        os << jsonString(values[i]);
    }
    os << "]";
    return os.str();
}

void writeConsumer(std::ostringstream& os, const StorageConsumer& c)
{
    os << "{\"path\":" << jsonString(c.path)
       << ",\"object_type\":" << jsonString(c.objectType)
       << ",\"logical_bytes\":" << jsonUInt(c.logicalBytes)
       << ",\"classification\":" << jsonString(c.classification)
       << ",\"confidence\":" << jsonString(c.confidence)
       << ",\"reclaimability\":" << jsonString(c.reclaimability)
       << ",\"location_safety\":" << jsonString(c.locationSafety)
       << ",\"candidate_strength\":" << jsonString(c.candidateStrength)
       << ",\"reason_codes\":" << jsonStringArray(c.reasonCodes) << "}";
}

void writeOpportunity(std::ostringstream& os, const OpportunityItem& item)
{
    os << "{\"path\":" << jsonString(item.path)
       << ",\"object_type\":" << jsonString(item.objectType)
       << ",\"logical_bytes\":" << jsonUInt(item.logicalBytes)
       << ",\"classification\":" << jsonString(item.classification)
       << ",\"confidence\":" << jsonString(item.confidence)
       << ",\"rule_id\":" << jsonString(item.ruleId)
       << ",\"reclaimability\":" << jsonString(item.reclaimability)
       << ",\"candidate_strength\":" << jsonString(item.candidateStrength)
       << ",\"location_safety\":" << jsonString(item.locationSafety) << ",";
    if (item.inactiveDays) {
        os << "\"inactive_days\":" << jsonUInt(*item.inactiveDays) << ",";
    } else {
        os << "\"inactive_days\":null,";
    }
    os << "\"activity_write_ticks\":" << jsonUInt(item.activityWriteTicks)
       << ",\"reason_codes\":" << jsonStringArray(item.reasonCodes)
       << ",\"explanation\":" << jsonString(item.explanation)
       << ",\"evidence\":{\"ecosystem\":" << jsonString(item.ecosystem)
       << ",\"marker\":" << jsonString(item.marker) << "}"
       << ",\"opportunity_rank\":" << jsonInt(item.opportunityRank)
       << ",\"overlapped\":" << jsonBool(item.overlapped) << "}";
}

void writeGroup(std::ostringstream& os, const OpportunityGroup& g)
{
    os << "{\"id\":" << jsonString(g.id)
       << ",\"classification\":" << jsonString(g.classification)
       << ",\"logical_bytes\":" << jsonUInt(g.logicalBytes)
       << ",\"item_count\":" << jsonUInt(g.itemCount)
       << ",\"estimated\":" << jsonBool(g.estimated)
       << ",\"strongest_candidate_strength\":"
       << jsonString(g.strongestCandidateStrength)
       << ",\"reason_codes\":" << jsonStringArray(g.reasonCodes) << "}";
}

}  // namespace

const char* toString(EvidenceSource source) noexcept
{
    switch (source) {
    case EvidenceSource::LiveScan:
        return "live_scan";
    case EvidenceSource::PersistentIndex:
        return "persistent_index";
    }
    return "live_scan";
}

const char* objectTypeName(ItemKind kind) noexcept
{
    switch (kind) {
    case ItemKind::File:
        return "file";
    case ItemKind::Directory:
    case ItemKind::ReparseDirectory:
        return "directory";
    }
    return "file";
}

const std::vector<std::string>& regenerableOpportunityClassifications()
{
    static const std::vector<std::string> kClasses = {
        "BuildArtifact", "DependencyDirectory", "PackageCache",
        "IdeCache",      "TemporaryData",       "LogData",
    };
    return kClasses;
}

std::string opportunityGroupId(StorageCategory category)
{
    switch (category) {
    case StorageCategory::DependencyDirectory:
        return "developer_dependencies";
    case StorageCategory::BuildArtifact:
        return "generated_outputs";
    case StorageCategory::PackageCache:
        return "package_cache";
    case StorageCategory::IdeCache:
        return "ide_cache";
    case StorageCategory::TemporaryData:
        return "temporary_data";
    case StorageCategory::LogData:
        return "log_data";
    default:
        return {};
    }
}

std::vector<std::string> reasonCodesFor(const ReclaimCandidate& candidate,
                                        bool oldLargeFile)
{
    std::vector<std::string> codes;
    auto add = [&](const char* code) {
        if (std::find(codes.begin(), codes.end(), code) == codes.end()) {
            codes.emplace_back(code);
        }
    };

    if (candidate.kind == ItemKind::Directory) {
        add(reason::kLargeDirectory);
    } else {
        add(reason::kLargeFile);
    }
    if (oldLargeFile) {
        add(reason::kOldLargeFile);
    }

    switch (candidate.classification.category) {
    case StorageCategory::DependencyDirectory:
        add(reason::kDeveloperDependency);
        if (candidate.classification.confidence == Confidence::High ||
            candidate.classification.confidence == Confidence::Medium) {
            add(reason::kKnownDependencyTree);
        }
        break;
    case StorageCategory::BuildArtifact:
        add(reason::kGeneratedOutput);
        if (candidate.classification.confidence == Confidence::High ||
            candidate.classification.confidence == Confidence::Medium) {
            add(reason::kKnownGeneratedOutput);
        }
        break;
    case StorageCategory::PackageCache:
        add(reason::kPackageCache);
        add(reason::kKnownPackageCache);
        break;
    case StorageCategory::IdeCache:
        add(reason::kIdeCache);
        break;
    case StorageCategory::TemporaryData:
        add(reason::kTemporaryData);
        break;
    case StorageCategory::LogData:
        add(reason::kLogData);
        break;
    case StorageCategory::UserData:
    case StorageCategory::Unknown:
    case StorageCategory::Archive:
    case StorageCategory::DownloadedAiModel:
    case StorageCategory::ApplicationData:
    case StorageCategory::SystemData:
        add(reason::kUserOrUnknownContent);
        break;
    }

    if (candidate.classification.ruleId == "known-temp-folder") {
        add(reason::kKnownTempLocation);
    }
    if (oldLargeFile &&
        candidate.classification.category == StorageCategory::Archive) {
        if (candidate.classification.ruleId == "installer-extension") {
            add(reason::kOldLargeInstaller);
        } else {
            add(reason::kOldLargeArchive);
        }
    }
    if (candidate.classification.marker == "downloads" ||
        pathIsUnderFolder(candidate.path, knownDownloadsFolder())) {
        add(reason::kDownloadsLocation);
    }

    if (candidate.reclaimability == Reclaimability::LikelyRegenerable) {
        add(reason::kLikelyRegenerable);
    } else if (candidate.reclaimability == Reclaimability::PossiblyRegenerable) {
        add(reason::kPossiblyRegenerable);
    }

    if (candidate.safety == LocationSafety::Protected) {
        add(reason::kProtectedLocation);
    } else if (candidate.safety == LocationSafety::Sensitive) {
        add(reason::kSensitiveLocation);
    } else if (candidate.safety == LocationSafety::Unknown) {
        add(reason::kUnknownLocation);
    }

    if (candidate.inactiveDays > 0 && candidate.inactiveDays < 90) {
        add(reason::kRecentActivity);
    }

    if (candidate.strength == CandidateStrength::Strong) {
        add(reason::kStrongCandidate);
    } else if (candidate.strength == CandidateStrength::Moderate) {
        add(reason::kModerateCandidate);
    } else if (candidate.strength == CandidateStrength::ReviewOnly) {
        add(reason::kReviewOnly);
    }
    return codes;
}

StorageOverviewReport buildLiveOverview(const ScanResult& result, std::size_t limit,
                                        FileTimeTicks nowTicks)
{
    StorageOverviewReport report;
    report.source = EvidenceSource::LiveScan;
    report.state = result.state == ScanState::Completed   ? "completed"
                   : result.state == ScanState::Cancelled ? "cancelled"
                   : result.state == ScanState::Failed    ? "failed"
                                                          : "unknown";
    report.ok = result.state == ScanState::Completed;
    report.files = result.progress.filesSeen;
    report.directories = result.progress.directoriesSeen;
    report.accessDenied = result.progress.accessDenied;
    report.reparseSkipped = result.progress.reparsePointsSkipped;
    report.otherErrors = result.progress.otherErrors;
    if (result.progress.elapsedSeconds > 0.0) {
        report.elapsedMs = static_cast<std::uint64_t>(
            result.progress.elapsedSeconds * 1000.0 + 0.5);
    }
    if (result.tree.empty()) {
        return report;
    }

    report.root = result.tree.pathOfDirectory(result.tree.root());
    report.logicalBytes = result.tree.dir(result.tree.root()).recursiveSize;

    // Root is always the largest directory. Fetch one extra slot so skipping
    // it still leaves a sentinel for truncated_directories.
    const auto dirs = topDirectories(result.tree, limit + 2);
    std::size_t dirMatches = 0;
    for (const auto& item : dirs) {
        if (normalizePathKey(item.path) == normalizePathKey(report.root)) {
            continue;
        }
        ++dirMatches;
        if (report.largestDirectories.size() >= limit) {
            continue;
        }
        // Reconstruct analysis for the directory path.
        const std::size_t n = result.tree.directoryCount();
        for (std::size_t i = 0; i < n; ++i) {
            const DirIndex idx = static_cast<DirIndex>(i);
            if (result.tree.pathOfDirectory(idx) == item.path) {
                report.largestDirectories.push_back(consumerFromCandidate(
                    analyzeDirectory(result.tree, idx, nowTicks),
                    reason::kLargeDirectory));
                break;
            }
        }
    }
    report.returnedDirectories = report.largestDirectories.size();
    report.truncatedDirectories = dirMatches > report.largestDirectories.size();

    std::vector<PathSizeItem> files = topFilesFromResult(result);
    if (files.size() < limit) {
        std::vector<PathSizeItem> all;
        const std::size_t n = result.tree.fileCount();
        all.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const FileIndex idx = static_cast<FileIndex>(i);
            all.push_back(PathSizeItem{result.tree.pathOfFile(idx),
                                       result.tree.file(idx).size});
        }
        std::sort(all.begin(), all.end(),
                  [](const PathSizeItem& a, const PathSizeItem& b) {
                      if (a.size_bytes != b.size_bytes) {
                          return a.size_bytes > b.size_bytes;
                      }
                      return a.path < b.path;
                  });
        files = std::move(all);
    }
    const std::size_t fileMatches = files.size();
    for (const auto& item : files) {
        if (report.largestFiles.size() >= limit) {
            break;
        }
        const std::size_t n = result.tree.fileCount();
        for (std::size_t i = 0; i < n; ++i) {
            const FileIndex idx = static_cast<FileIndex>(i);
            if (result.tree.pathOfFile(idx) == item.path) {
                report.largestFiles.push_back(consumerFromCandidate(
                    analyzeFileEntry(result.tree, idx, nowTicks),
                    reason::kLargeFile));
                break;
            }
        }
    }
    report.returnedFiles = report.largestFiles.size();
    report.truncatedFiles = fileMatches > report.largestFiles.size();

    OpportunityQuery summaryQuery;
    summaryQuery.minSize = kDefaultOpportunityMinSize;
    summaryQuery.olderThanDays = kDefaultOldLargeDays;
    summaryQuery.nowTicks = nowTicks;
    summaryQuery.limit = 1;
    report.opportunitySummary = buildLiveOpportunities(result.tree, summaryQuery).groups;
    return report;
}

StorageOverviewReport buildIndexedOverview(
    const std::wstring& root, ByteSize logicalBytes, std::uint64_t files,
    std::uint64_t directories, const std::vector<IndexHit>& directoryHits,
    const std::vector<IndexHit>& fileHits, std::uint64_t indexAgeMs,
    std::string indexedAtIso, std::size_t limit)
{
    StorageOverviewReport report;
    report.source = EvidenceSource::PersistentIndex;
    report.root = root;
    report.ok = true;
    report.state = "completed";
    report.logicalBytes = logicalBytes;
    report.files = files;
    report.directories = directories;
    report.indexAgeMs = indexAgeMs;
    report.indexedAtIso = std::move(indexedAtIso);

    std::size_t dirSeen = 0;
    for (const auto& hit : directoryHits) {
        if (normalizePathKey(hit.path) == normalizePathKey(root)) {
            continue;
        }
        ++dirSeen;
        if (report.largestDirectories.size() >= limit) {
            continue;
        }
        auto candidate = candidateFromHit(hit, 0);
        report.largestDirectories.push_back(
            consumerFromCandidate(candidate, reason::kLargeDirectory));
    }
    report.returnedDirectories = report.largestDirectories.size();
    report.truncatedDirectories = dirSeen > report.largestDirectories.size();

    std::size_t fileSeen = 0;
    for (const auto& hit : fileHits) {
        ++fileSeen;
        if (report.largestFiles.size() >= limit) {
            continue;
        }
        auto candidate = candidateFromHit(hit, 0);
        report.largestFiles.push_back(
            consumerFromCandidate(candidate, reason::kLargeFile));
    }
    report.returnedFiles = report.largestFiles.size();
    report.truncatedFiles = fileSeen > report.largestFiles.size();
    return report;
}

OpportunityReport buildLiveOpportunities(const DirectoryTree& tree,
                                         const OpportunityQuery& query)
{
    OpportunityReport report;
    report.source = EvidenceSource::LiveScan;
    fillTreeSummary(report, tree);
    if (tree.empty()) {
        return report;
    }

    std::vector<ReclaimCandidate> selected;
    const std::size_t dirs = tree.directoryCount();
    for (std::size_t i = 0; i < dirs; ++i) {
        const DirIndex idx = static_cast<DirIndex>(i);
        if (idx == tree.root()) {
            continue;
        }
        auto candidate = analyzeDirectory(tree, idx, query.nowTicks);
        if (includeOpportunity(candidate, query)) {
            selected.push_back(std::move(candidate));
        }
    }
    const std::size_t files = tree.fileCount();
    for (std::size_t i = 0; i < files; ++i) {
        const FileIndex idx = static_cast<FileIndex>(i);
        auto candidate = analyzeFileEntry(tree, idx, query.nowTicks);
        if (includeOpportunity(candidate, query)) {
            selected.push_back(std::move(candidate));
        }
    }

    report.opportunities.reserve(selected.size());
    for (const auto& candidate : selected) {
        report.opportunities.push_back(
            itemFromCandidate(candidate, isOldLargeFile(candidate, query)));
    }
    markOverlaps(report.opportunities);
    buildGroupsAndUniqueBytes(report);
    assignRanksAndBounds(report, query.limit);
    // unique bytes / groups are computed on the pre-truncation set so a
    // --limit does not hide an aggregate that still exists in the tree.
    return report;
}

OpportunityReport buildIndexedOpportunities(
    const std::wstring& root, ByteSize logicalBytes, std::uint64_t files,
    std::uint64_t directories, const std::vector<IndexHit>& hits,
    const OpportunityQuery& query, std::uint64_t indexAgeMs,
    std::string indexedAtIso, IndexedOpportunityExtras extras)
{
    OpportunityReport report;
    report.source = EvidenceSource::PersistentIndex;
    report.root = root;
    report.logicalBytes = logicalBytes;
    report.files = files;
    report.directories = directories;
    report.indexAgeMs = indexAgeMs;
    report.indexedAtIso = std::move(indexedAtIso);

    const std::vector<IndexHit>* sourceHits = &hits;
    if (extras.aggregateHits != nullptr && !extras.aggregatesCapped &&
        !extras.aggregateHits->empty()) {
        sourceHits = extras.aggregateHits;
    }

    std::vector<OpportunityItem> items;
    items.reserve(sourceHits->size());
    for (const auto& hit : *sourceHits) {
        if (normalizePathKey(hit.path) == normalizePathKey(root)) {
            continue;
        }
        auto candidate = candidateFromHit(hit, query.nowTicks);
        if (!includeOpportunity(candidate, query)) {
            continue;
        }
        items.push_back(
            itemFromCandidate(candidate, isOldLargeFile(candidate, query)));
    }
    report.opportunities = std::move(items);
    markOverlaps(report.opportunities);
    buildGroupsAndUniqueBytes(report);
    assignRanksAndBounds(report, query.limit);
    if (extras.matchedCount > 0) {
        report.truncated = extras.matchedCount > query.limit;
    }
    if (extras.aggregatesCapped) {
        report.uniqueReviewEstimated = true;
        for (auto& group : report.groups) {
            group.estimated = true;
        }
    }
    if (report.logicalBytes > 0 &&
        report.uniqueReviewBytes > report.logicalBytes) {
        report.uniqueReviewBytes = report.logicalBytes;
        report.uniqueReviewEstimated = true;
    }
    return report;
}

std::string StorageOverviewReport::toJson() const
{
    std::ostringstream os;
    os << "{"
       << "\"schema_version\":" << schemaVersion << ","
       << "\"ok\":" << jsonBool(ok) << ","
       << "\"command\":\"overview\","
       << "\"source\":" << jsonString(toString(source)) << ","
       << "\"root\":" << jsonString(root) << ","
       << "\"state\":" << jsonString(state) << ","
       << "\"summary\":{"
       << "\"logical_bytes\":" << jsonUInt(logicalBytes) << ","
       << "\"files\":" << jsonUInt(files) << ","
       << "\"directories\":" << jsonUInt(directories)
       << "},"
       << "\"largest_directories\":[";
    for (std::size_t i = 0; i < largestDirectories.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        writeConsumer(os, largestDirectories[i]);
    }
    os << "],\"largest_files\":[";
    for (std::size_t i = 0; i < largestFiles.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        writeConsumer(os, largestFiles[i]);
    }
    os << "],\"opportunity_summary\":[";
    for (std::size_t i = 0; i < opportunitySummary.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        writeGroup(os, opportunitySummary[i]);
    }
    os << "],"
       << "\"returned_directories\":" << jsonUInt(returnedDirectories) << ","
       << "\"returned_files\":" << jsonUInt(returnedFiles) << ","
       << "\"truncated_directories\":" << jsonBool(truncatedDirectories) << ","
       << "\"truncated_files\":" << jsonBool(truncatedFiles) << ","
       << "\"access_denied\":" << jsonUInt(accessDenied) << ","
       << "\"reparse_skipped\":" << jsonUInt(reparseSkipped) << ","
       << "\"other_errors\":" << jsonUInt(otherErrors) << ","
       << "\"elapsed_ms\":" << jsonUInt(elapsedMs);
    if (source == EvidenceSource::PersistentIndex) {
        os << ",\"index\":{"
           << "\"age_ms\":" << jsonUInt(indexAgeMs) << ","
           << "\"indexed_at\":" << jsonString(indexedAtIso) << "}";
    }
    if (!error.empty()) {
        os << ",\"error\":" << jsonString(error);
    }
    os << ",\"read_only\":true,\"filesystem_mutation\":false}\n";
    return os.str();
}

std::string OpportunityReport::toJson() const
{
    std::ostringstream os;
    os << "{"
       << "\"schema_version\":" << schemaVersion << ","
       << "\"ok\":" << jsonBool(ok) << ","
       << "\"command\":\"opportunities\","
       << "\"source\":" << jsonString(toString(source)) << ","
       << "\"root\":" << jsonString(root) << ","
       << "\"state\":" << jsonString(state) << ","
       << "\"planning_only\":true,"
       << "\"read_only\":true,"
       << "\"filesystem_mutation\":false,"
       << "\"ranking_policy\":" << jsonString(rankingPolicy) << ","
       << "\"summary\":{"
       << "\"logical_bytes\":" << jsonUInt(logicalBytes) << ","
       << "\"files\":" << jsonUInt(files) << ","
       << "\"directories\":" << jsonUInt(directories) << ","
       << "\"unique_review_bytes\":" << jsonUInt(uniqueReviewBytes) << ","
       << "\"unique_review_estimated\":" << jsonBool(uniqueReviewEstimated) << ","
       << "\"returned_count\":" << jsonUInt(returnedCount) << ","
       << "\"truncated\":" << jsonBool(truncated)
       << "},"
       << "\"groups\":[";
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        writeGroup(os, groups[i]);
    }
    os << "],\"opportunities\":[";
    for (std::size_t i = 0; i < opportunities.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        writeOpportunity(os, opportunities[i]);
    }
    os << "],"
       << "\"access_denied\":" << jsonUInt(accessDenied) << ","
       << "\"reparse_skipped\":" << jsonUInt(reparseSkipped) << ","
       << "\"other_errors\":" << jsonUInt(otherErrors) << ","
       << "\"elapsed_ms\":" << jsonUInt(elapsedMs);
    if (source == EvidenceSource::PersistentIndex) {
        os << ",\"index\":{"
           << "\"age_ms\":" << jsonUInt(indexAgeMs) << ","
           << "\"indexed_at\":" << jsonString(indexedAtIso) << "}";
    }
    if (!error.empty()) {
        os << ",\"error\":" << jsonString(error);
    }
    os << "}\n";
    return os.str();
}

}  // namespace spacelens
