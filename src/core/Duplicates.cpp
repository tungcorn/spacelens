#include "core/Duplicates.hpp"

#include "core/CleanupPlan.hpp"
#include "core/Json.hpp"
#include "core/SafetyPolicy.hpp"
#include "core/SizeFormatter.hpp"
#include "core/index/IndexStore.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <sstream>
#include <utility>

namespace spacelens {
namespace {

constexpr ByteSize kMaxBytes = std::numeric_limits<ByteSize>::max();

std::wstring comparePath(std::wstring_view path)
{
    return normalizeCleanupPath(path);
}

std::string hexByte(std::uint8_t value)
{
    char buffer[3]{};
    std::snprintf(buffer, sizeof(buffer), "%02x", value);
    return buffer;
}

void appendSha256(std::ostringstream& os, const std::array<std::uint8_t, 32>& digest)
{
    for (const auto byte : digest) {
        os << hexByte(byte);
    }
}

std::wstring displayPath(std::wstring_view path, std::wstring_view profile)
{
    if (profile.empty()) {
        return std::wstring(path);
    }
    return redactUserProfilePath(path, profile);
}

void sortInstancePaths(DuplicateContentInstance& instance)
{
    std::sort(instance.paths.begin(), instance.paths.end(),
              [](const DuplicatePathRecord& a, const DuplicatePathRecord& b) {
                  const auto left = comparePath(a.path);
                  const auto right = comparePath(b.path);
                  if (left != right) {
                      return left < right;
                  }
                  return a.path < b.path;
              });
    bool first = true;
    for (auto& path : instance.paths) {
        path.hardLinkAlias = !first;
        first = false;
    }
}

const DuplicatePathRecord* firstPath(const DuplicateGroup& group)
{
    for (const auto& instance : group.instances) {
        if (!instance.paths.empty()) {
            return &instance.paths.front();
        }
    }
    return nullptr;
}

}  // namespace

const char* toString(DuplicateFileStatus status) noexcept
{
    switch (status) {
    case DuplicateFileStatus::Candidate:
        return "Candidate";
    case DuplicateFileStatus::Verified:
        return "Verified";
    case DuplicateFileStatus::SameIdentity:
        return "SameIdentity";
    case DuplicateFileStatus::Missing:
        return "Missing";
    case DuplicateFileStatus::AccessDenied:
        return "AccessDenied";
    case DuplicateFileStatus::ReparsePoint:
        return "ReparsePoint";
    case DuplicateFileStatus::NotRegularFile:
        return "NotRegularFile";
    case DuplicateFileStatus::SizeChanged:
        return "SizeChanged";
    case DuplicateFileStatus::ChangedDuringRead:
        return "ChangedDuringRead";
    case DuplicateFileStatus::IdentityChanged:
        return "IdentityChanged";
    case DuplicateFileStatus::ReadError:
        return "ReadError";
    case DuplicateFileStatus::Cancelled:
        return "Cancelled";
    case DuplicateFileStatus::Unsupported:
        return "Unsupported";
    case DuplicateFileStatus::EmptyIgnored:
        return "EmptyIgnored";
    }
    return "ReadError";
}

ByteSize saturatingMul(ByteSize a, ByteSize b, bool& saturated) noexcept
{
    if (a == 0 || b == 0) {
        return 0;
    }
    if (a > kMaxBytes / b) {
        saturated = true;
        return kMaxBytes;
    }
    return a * b;
}

ByteSize redundantLogicalBytes(std::size_t distinctInstances,
                               ByteSize logicalSize,
                               bool& saturated) noexcept
{
    if (distinctInstances <= 1) {
        return 0;
    }
    return saturatingMul(static_cast<ByteSize>(distinctInstances - 1U), logicalSize,
                         saturated);
}

ByteSize redundantLogicalBytes(std::size_t distinctInstances,
                               ByteSize logicalSize) noexcept
{
    bool saturated = false;
    return redundantLogicalBytes(distinctInstances, logicalSize, saturated);
}

void finalizeDuplicateGroup(DuplicateGroup& group) noexcept
{
    std::sort(group.instances.begin(), group.instances.end(),
              [](const DuplicateContentInstance& a,
                 const DuplicateContentInstance& b) {
                  const auto left =
                      a.paths.empty() ? std::wstring{} : comparePath(a.paths.front().path);
                  const auto right =
                      b.paths.empty() ? std::wstring{} : comparePath(b.paths.front().path);
                  if (left != right) {
                      return left < right;
                  }
                  return duplicateIdentityKey(a.identity) <
                         duplicateIdentityKey(b.identity);
              });

    group.pathCount = 0;
    group.hardLinkAliasPathCount = 0;
    group.distinctIdentityCount = group.instances.size();
    for (auto& instance : group.instances) {
        sortInstancePaths(instance);
        group.pathCount += instance.paths.size();
        if (instance.paths.size() > 1) {
            group.hardLinkAliasPathCount += instance.paths.size() - 1;
        }
    }
    group.redundantCopyCount =
        group.distinctIdentityCount == 0 ? 0 : group.distinctIdentityCount - 1;
    group.redundantBytesSaturated = false;
    group.potentialRedundantLogicalBytes = redundantLogicalBytes(
        group.distinctIdentityCount, group.logicalSize, group.redundantBytesSaturated);
}

void sortDuplicateGroups(std::vector<DuplicateGroup>& groups)
{
    std::sort(groups.begin(), groups.end(),
              [](const DuplicateGroup& a, const DuplicateGroup& b) {
                  if (a.potentialRedundantLogicalBytes !=
                      b.potentialRedundantLogicalBytes) {
                      return a.potentialRedundantLogicalBytes >
                             b.potentialRedundantLogicalBytes;
                  }
                  if (a.logicalSize != b.logicalSize) {
                      return a.logicalSize > b.logicalSize;
                  }
                  if (a.contentSha256Hex != b.contentSha256Hex) {
                      return a.contentSha256Hex < b.contentSha256Hex;
                  }
                  const auto* left = firstPath(a);
                  const auto* right = firstPath(b);
                  const auto leftPath =
                      left == nullptr ? std::wstring{} : comparePath(left->path);
                  const auto rightPath =
                      right == nullptr ? std::wstring{} : comparePath(right->path);
                  if (leftPath != rightPath) {
                      return leftPath < rightPath;
                  }
                  return a.verification < b.verification;
              });
}

void accumulateDuplicateSummary(DuplicateDetectionResult& result) noexcept
{
    result.summary = {};
    result.summary.candidateFiles = result.progress.candidateFiles;
    result.summary.candidateBytes = result.progress.candidateBytes;
    result.summary.bytesRead = result.progress.bytesRead;
    result.summary.bytesFullyHashed = result.progress.bytesFullyHashed;
    result.summary.bytesReusedFromCache = result.progress.bytesReusedFromCache;
    result.summary.cacheHits = result.progress.cacheHits;
    result.summary.cacheMisses = result.progress.cacheMisses;
    result.summary.cacheInvalidations = result.progress.cacheInvalidations;
    result.summary.cacheWrites = result.progress.cacheWrites;
    result.summary.filesFullyHashed = result.progress.filesFullyHashed;
    result.summary.skippedFiles = result.skipped.size();
    result.summary.verifiedGroups = result.groups.size();

    bool saturated = false;
    ByteSize redundant = 0;
    for (const auto& group : result.groups) {
        result.summary.verifiedPaths += group.pathCount;
        result.summary.distinctContentInstances += group.distinctIdentityCount;
        result.summary.hardLinkAliasPaths += group.hardLinkAliasPathCount;
        if (group.redundantBytesSaturated) {
            saturated = true;
        }
        if (kMaxBytes - redundant < group.potentialRedundantLogicalBytes) {
            redundant = kMaxBytes;
            saturated = true;
        } else {
            redundant += group.potentialRedundantLogicalBytes;
        }
    }
    result.summary.potentialRedundantLogicalBytes = redundant;
    result.summary.redundantBytesSaturated = saturated;
}

std::string sha256ToHex(const std::array<std::uint8_t, 32>& digest)
{
    std::ostringstream os;
    appendSha256(os, digest);
    return os.str();
}

std::string duplicateIdentityKey(const CleanupIdentity& identity)
{
    if (!isIdentityAvailable(identity)) {
        return {};
    }
    std::ostringstream os;
    os << toString(identity.source) << ':' << identity.volumeSerial << ':';
    if (identity.source == CleanupIdentitySource::FileIndex64Fallback) {
        os << identity.fileIndex64;
    } else {
        for (const auto byte : identity.fileId128) {
            os << hexByte(byte);
        }
    }
    return os.str();
}

CleanupCandidate cleanupCandidateFromDuplicate(
    const DuplicateGroup& group,
    const DuplicateContentInstance& instance,
    const DuplicatePathRecord& path,
    const std::wstring& sourceRoot,
    std::uint64_t indexAgeMs,
    const std::string& indexedAtIso)
{
    CleanupCandidate candidate;
    candidate.path = path.path;
    candidate.kind = ItemKind::File;
    candidate.sizeAtSelection = group.logicalSize;
    candidate.lastWriteTime = path.lastWrite;
    candidate.attributes = path.attributes;
    candidate.objectEvidence.available = true;
    candidate.objectEvidence.identity = instance.identity;
    candidate.objectEvidence.kind = ItemKind::File;
    candidate.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
    candidate.objectEvidence.logicalSize = group.logicalSize;
    candidate.objectEvidence.lastWriteTime = path.lastWrite;
    candidate.objectEvidence.attributes = path.attributes;
    candidate.capturedSafety = classifyLocation(path.path);
    candidate.sourceRoot = sourceRoot;
    candidate.source = "duplicate_detection";
    candidate.indexAgeMs = indexAgeMs;
    candidate.indexIndexedAtIso = indexedAtIso;
    std::ostringstream reason;
    reason << "Verified duplicate content from Duplicate Detection V1";
    if (!group.contentSha256Hex.empty()) {
        reason << "; sha256=" << group.contentSha256Hex;
    }
    reason << "; verification=" << group.verification;
    reason << "; independent_copies=" << group.distinctIdentityCount;
    candidate.reasonAdded = reason.str();
    candidate.classification.reason = candidate.reasonAdded;
    return candidate;
}

std::string DuplicateDetectionResult::toText(const DuplicateScanOptions& options) const
{
    std::ostringstream os;
    os << "SpaceLens Duplicate Detection\n";
    os << "Planning only — no files will be deleted, moved, or linked.\n";
    os << "Root: " << utf8FromWide(displayPath(root, options.userProfilePath)) << "\n";
    os << "Minimum size: " << SizeFormatter::format(minimumSize) << "\n";
    os << "Source: " << source << "\n";
    os << "Verification: " << verification << "\n";
    os << "Completed: " << (completed ? "yes" : "no");
    if (cancelled) {
        os << " (cancelled / partial)";
    }
    os << "\n";
    if (!error.empty()) {
        os << "Error: " << error << "\n";
    }
    os << "Verified groups: " << summary.verifiedGroups << "\n";
    os << "Verified paths: " << summary.verifiedPaths << "\n";
    os << "Independent content instances: " << summary.distinctContentInstances
       << "\n";
    os << "Hard-link alias paths: " << summary.hardLinkAliasPaths << "\n";
    os << "Potential redundant logical bytes: "
       << SizeFormatter::format(summary.potentialRedundantLogicalBytes);
    if (summary.redundantBytesSaturated) {
        os << " (saturated)";
    }
    os << "\n";
    if (summary.cacheHits != 0 || summary.cacheMisses != 0 ||
        summary.cacheWrites != 0 || summary.cacheInvalidations != 0) {
        os << "Hash cache: " << summary.cacheHits << " hit(s), "
           << summary.cacheMisses << " miss(es), " << summary.cacheWrites
           << " write(s)";
        if (summary.cacheInvalidations != 0) {
            os << ", " << summary.cacheInvalidations << " invalid row(s)";
        }
        os << "\n";
    }
    os << "Same-size groups are not duplicates. Sample fingerprints are not "
          "proof of equality.\n\n";

    for (const auto& group : groups) {
        const auto* lead = firstPath(group);
        os << (lead == nullptr
                   ? std::string("(unnamed)")
                   : utf8FromWide(displayPath(lead->path, options.userProfilePath)))
           << "\n";
        os << "  " << SizeFormatter::format(group.logicalSize) << " × "
           << group.distinctIdentityCount << " independent cop"
           << (group.distinctIdentityCount == 1 ? "y" : "ies") << "\n";
        os << "  Potential redundant: "
           << SizeFormatter::format(group.potentialRedundantLogicalBytes) << "\n";
        if (group.verification == "same_file_identity") {
            os << "  Verification: same file identity (hard-link aliases)\n";
        } else if (!group.contentSha256Hex.empty()) {
            os << "  Verified SHA-256: " << group.contentSha256Hex << "\n";
        }
        if (group.hardLinkAliasPathCount > 0) {
            os << "  Hard-link aliases: " << group.hardLinkAliasPathCount << "\n";
        }
        for (const auto& instance : group.instances) {
            for (const auto& path : instance.paths) {
                os << "    " << utf8FromWide(displayPath(path.path, options.userProfilePath));
                if (path.hardLinkAlias) {
                    os << "  [hard-link alias]";
                }
                os << "\n";
            }
        }
        os << "\n";
    }

    if (!skipped.empty()) {
        os << "Skipped / inconclusive:\n";
        for (const auto& skip : skipped) {
            os << "  " << utf8FromWide(displayPath(skip.path, options.userProfilePath))
               << "  " << toString(skip.status);
            if (!skip.detail.empty()) {
                os << " — " << skip.detail;
            }
            os << "\n";
        }
    }
    return os.str();
}

std::string DuplicateDetectionResult::toJson(const DuplicateScanOptions& options) const
{
    std::ostringstream os;
    os << "{"
       << "\"schema_version\":" << schemaVersion << ","
       << "\"source\":" << jsonString(source) << ","
       << "\"verification\":" << jsonString(verification) << ","
       << "\"planning_only\":" << jsonBool(planningOnly) << ","
       << "\"read_only\":" << jsonBool(readOnly) << ","
       << "\"filesystem_mutation\":" << jsonBool(filesystemMutation) << ","
       << "\"root\":" << jsonString(displayPath(root, options.userProfilePath)) << ","
       << "\"generated_at\":" << jsonString(generatedAt) << ","
       << "\"minimum_size\":" << jsonUInt(minimumSize) << ","
       << "\"completed\":" << jsonBool(completed) << ","
       << "\"cancelled\":" << jsonBool(cancelled) << ","
       << "\"error\":" << jsonString(error) << ","
       << "\"index_age_ms\":" << jsonUInt(indexAgeMs) << ","
       << "\"index_indexed_at\":" << jsonString(indexIndexedAtIso) << ","
       << "\"summary\":{"
       << "\"verified_groups\":" << jsonUInt(summary.verifiedGroups) << ","
       << "\"verified_paths\":" << jsonUInt(summary.verifiedPaths) << ","
       << "\"distinct_content_instances\":"
       << jsonUInt(summary.distinctContentInstances) << ","
       << "\"hardlink_alias_paths\":" << jsonUInt(summary.hardLinkAliasPaths) << ","
       << "\"potential_redundant_logical_bytes\":"
       << jsonUInt(summary.potentialRedundantLogicalBytes) << ","
       << "\"redundant_bytes_saturated\":" << jsonBool(summary.redundantBytesSaturated)
       << ","
       << "\"bytes_read\":" << jsonUInt(summary.bytesRead) << ","
       << "\"bytes_fully_hashed\":" << jsonUInt(summary.bytesFullyHashed) << ","
       << "\"bytes_reused_from_cache\":"
       << jsonUInt(summary.bytesReusedFromCache) << ","
       << "\"cache_hits\":" << jsonUInt(summary.cacheHits) << ","
       << "\"cache_misses\":" << jsonUInt(summary.cacheMisses) << ","
       << "\"cache_invalidations\":" << jsonUInt(summary.cacheInvalidations)
       << ","
       << "\"cache_writes\":" << jsonUInt(summary.cacheWrites) << ","
       << "\"files_fully_hashed\":" << jsonUInt(summary.filesFullyHashed) << ","
       << "\"candidate_files\":" << jsonUInt(summary.candidateFiles) << ","
       << "\"candidate_bytes\":" << jsonUInt(summary.candidateBytes) << ","
       << "\"skipped_files\":" << jsonUInt(summary.skippedFiles) << "},"
       << "\"groups\":[";
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto& group = groups[i];
        if (i != 0) {
            os << ',';
        }
        os << "{"
           << "\"full_hash\":" << jsonString(group.contentSha256Hex) << ","
           << "\"verification\":" << jsonString(group.verification) << ","
           << "\"logical_size\":" << jsonUInt(group.logicalSize) << ","
           << "\"distinct_identity_count\":" << jsonUInt(group.distinctIdentityCount)
           << ","
           << "\"path_count\":" << jsonUInt(group.pathCount) << ","
           << "\"hardlink_alias_paths\":" << jsonUInt(group.hardLinkAliasPathCount)
           << ","
           << "\"redundant_copy_count\":" << jsonUInt(group.redundantCopyCount) << ","
           << "\"potential_redundant_logical_bytes\":"
           << jsonUInt(group.potentialRedundantLogicalBytes) << ","
           << "\"instances\":[";
        for (std::size_t j = 0; j < group.instances.size(); ++j) {
            const auto& instance = group.instances[j];
            if (j != 0) {
                os << ',';
            }
            os << "{"
               << "\"identity_source\":"
               << jsonString(toString(instance.identity.source)) << ","
               << "\"identity_key\":"
               << jsonString(duplicateIdentityKey(instance.identity)) << ","
               << "\"paths\":[";
            for (std::size_t k = 0; k < instance.paths.size(); ++k) {
                const auto& path = instance.paths[k];
                if (k != 0) {
                    os << ',';
                }
                os << "{"
                   << "\"path\":"
                   << jsonString(displayPath(path.path, options.userProfilePath))
                   << ","
                   << "\"status\":" << jsonString(toString(path.status)) << ","
                   << "\"hardlink_alias\":" << jsonBool(path.hardLinkAlias) << "}";
            }
            os << "]}";
        }
        os << "]}";
    }
    os << "],\"skipped\":[";
    for (std::size_t i = 0; i < skipped.size(); ++i) {
        const auto& skip = skipped[i];
        if (i != 0) {
            os << ',';
        }
        os << "{"
           << "\"path\":" << jsonString(displayPath(skip.path, options.userProfilePath))
           << ","
           << "\"indexed_size\":" << jsonUInt(skip.indexedSize) << ","
           << "\"status\":" << jsonString(toString(skip.status)) << ","
           << "\"detail\":" << jsonString(skip.detail) << "}";
    }
    os << "]}";
    return os.str();
}

}  // namespace spacelens
