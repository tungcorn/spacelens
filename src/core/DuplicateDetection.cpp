#include "core/DuplicateDetection.hpp"

#include "core/Json.hpp"
#include "core/index/IndexStore.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace spacelens {
namespace {

bool stopRequested(const std::function<bool()>& cancelled)
{
    return static_cast<bool>(cancelled) && cancelled();
}

void emitProgress(const std::function<void(const DuplicateScanProgress&)>& onProgress,
                  const DuplicateScanProgress& progress)
{
    if (onProgress) {
        onProgress(progress);
    }
}

std::string instanceKey(const CleanupIdentity& identity, std::wstring_view path)
{
    auto key = duplicateIdentityKey(identity);
    if (!key.empty()) {
        return key;
    }
    return std::string("path:") + utf8FromWide(normalizeCleanupPath(path));
}

struct LiveFile {
    DuplicateIndexCandidate index{};
    CleanupIdentity identity{};
    FileTimeTicks lastWrite = 0;
    std::uint32_t attributes = 0;
    DuplicateFileStatus status = DuplicateFileStatus::Candidate;
};

struct IdentityWork {
    std::string key;
    CleanupIdentity identity{};
    std::vector<LiveFile*> files;
    std::array<std::uint8_t, 32> sample{};
    std::array<std::uint8_t, 32> full{};
    bool hasSample = false;
    bool hasFull = false;
    DuplicateFileStatus status = DuplicateFileStatus::Candidate;
    std::string detail;
};

void addSkip(DuplicateDetectionResult& result,
             const DuplicateIndexCandidate& file,
             DuplicateFileStatus status,
             std::string detail)
{
    DuplicateSkip skip;
    skip.path = file.path;
    skip.indexedSize = file.logicalSize;
    skip.status = status;
    skip.detail = std::move(detail);
    result.skipped.push_back(std::move(skip));
    ++result.progress.skippedFiles;
}

void skipFiles(DuplicateDetectionResult& result,
               const std::vector<LiveFile*>& files,
               DuplicateFileStatus status,
               const std::string& detail)
{
    for (LiveFile* file : files) {
        if (file == nullptr) {
            continue;
        }
        file->status = status;
        addSkip(result, file->index, status, detail);
    }
}

DuplicateGroup makeGroup(std::vector<IdentityWork*> works,
                         const std::string& hashHex,
                         const std::string& verification,
                         ByteSize logicalSize,
                         FileTimeTicks verifiedAt)
{
    DuplicateGroup group;
    group.contentSha256Hex = hashHex;
    group.verification = verification;
    group.logicalSize = logicalSize;
    group.verifiedAt = verifiedAt;
    const DuplicateFileStatus pathStatus =
        verification == "same_file_identity" ? DuplicateFileStatus::SameIdentity
                                             : DuplicateFileStatus::Verified;
    for (IdentityWork* work : works) {
        if (work == nullptr) {
            continue;
        }
        DuplicateContentInstance instance;
        instance.identity = work->identity;
        instance.logicalSize = logicalSize;
        for (LiveFile* file : work->files) {
            if (file == nullptr) {
                continue;
            }
            DuplicatePathRecord record;
            record.path = file->index.path;
            record.status = pathStatus;
            record.lastWrite = file->lastWrite;
            record.attributes = file->attributes;
            instance.paths.push_back(std::move(record));
        }
        if (!instance.paths.empty()) {
            group.instances.push_back(std::move(instance));
        }
    }
    finalizeDuplicateGroup(group);
    return group;
}

ContentHashResult hashIdentity(IFileContentHasher& hasher,
                               IdentityWork& work,
                               ByteSize logicalSize,
                               ContentHashKind kind,
                               const std::function<bool()>& cancelled)
{
    ContentHashRequest request;
    request.path = work.files.front()->index.path;
    request.expectedSize = logicalSize;
    request.expectedIdentity = work.identity;
    request.expectedLastWrite = work.files.front()->lastWrite;
    request.kind = kind;
    request.cancelled = cancelled;
    return hasher.hash(request);
}

}  // namespace

DuplicateDetectionResult detectDuplicates(
    const DuplicateCandidateQueryResult& candidates,
    ICleanupMetadataReader& reader,
    IFileContentHasher& hasher,
    const DuplicateScanOptions& options,
    const std::function<bool()>& cancelled,
    const std::function<void(const DuplicateScanProgress&)>& onProgress)
{
    DuplicateDetectionResult result;
    result.minimumSize = options.minimumSize;
    result.root = candidates.root.rootPath.empty() ? candidates.location.rootPath
                                                   : candidates.root.rootPath;
    result.indexAgeMs = candidates.ageMs;
    result.indexIndexedAtIso = candidates.root.indexedAtIso;
    result.generatedAt = options.generatedAt.empty()
                             ? fileTimeTicksToIsoUtc(0)
                             : options.generatedAt;
    if (options.generatedAt.empty()) {
        result.generatedAt = "1970-01-01T00:00:00Z";
    }
    result.progress.candidateFiles = candidates.candidateFiles;
    result.progress.candidateBytes = candidates.candidateBytes;

    if (!candidates.ok) {
        result.error = candidates.error.empty() ? "index_query_failed"
                                                : candidates.error;
        accumulateDuplicateSummary(result);
        return result;
    }

    for (const auto& bucket : candidates.buckets) {
        if (stopRequested(cancelled)) {
            result.cancelled = true;
            break;
        }
        if (bucket.logicalSize == 0 || bucket.files.size() < 2) {
            continue;
        }

        std::vector<LiveFile> live;
        live.reserve(bucket.files.size());
        for (const auto& file : bucket.files) {
            if (stopRequested(cancelled)) {
                result.cancelled = true;
                addSkip(result, file, DuplicateFileStatus::Cancelled,
                        "Scan cancelled");
                continue;
            }
            if (file.logicalSize == 0) {
                addSkip(result, file, DuplicateFileStatus::EmptyIgnored,
                        "Zero-length files are ignored");
                ++result.progress.filesProbed;
                emitProgress(onProgress, result.progress);
                continue;
            }

            const CleanupMetadataProbe probe = reader.read(file.path);
            ++result.progress.filesProbed;
            emitProgress(onProgress, result.progress);

            if (probe.outcome == CleanupMetadataProbeOutcome::Missing) {
                addSkip(result, file, DuplicateFileStatus::Missing,
                        probe.detail.empty() ? "Path not found" : probe.detail);
                continue;
            }
            if (probe.outcome == CleanupMetadataProbeOutcome::AccessDenied) {
                addSkip(result, file, DuplicateFileStatus::AccessDenied,
                        probe.detail.empty() ? "Access denied" : probe.detail);
                continue;
            }
            if (probe.outcome != CleanupMetadataProbeOutcome::Present ||
                !probe.objectEvidence.available) {
                addSkip(result, file, DuplicateFileStatus::ReadError,
                        probe.detail.empty() ? "Metadata probe failed"
                                             : probe.detail);
                continue;
            }
            if (probe.isReparse ||
                probe.objectEvidence.kind == ItemKind::ReparseDirectory) {
                addSkip(result, file, DuplicateFileStatus::ReparsePoint,
                        "Reparse points are not hashed");
                continue;
            }
            if (probe.objectEvidence.kind != ItemKind::File) {
                addSkip(result, file, DuplicateFileStatus::NotRegularFile,
                        "Only regular files are duplicate candidates");
                continue;
            }
            if (probe.objectEvidence.logicalSize != bucket.logicalSize) {
                addSkip(result, file, DuplicateFileStatus::SizeChanged,
                        "Live size no longer matches the indexed size group");
                continue;
            }

            LiveFile item;
            item.index = file;
            item.identity = probe.objectEvidence.identity;
            item.lastWrite = probe.objectEvidence.lastWriteTime;
            item.attributes = probe.objectEvidence.attributes;
            live.push_back(std::move(item));
        }

        if (result.cancelled) {
            break;
        }

        std::map<std::string, IdentityWork> works;
        for (auto& item : live) {
            IdentityWork& work = works[instanceKey(item.identity, item.index.path)];
            if (work.key.empty()) {
                work.key = instanceKey(item.identity, item.index.path);
                work.identity = item.identity;
            }
            work.files.push_back(&item);
        }

        std::vector<IdentityWork*> pending;
        pending.reserve(works.size());
        for (auto& [_, work] : works) {
            pending.push_back(&work);
        }

        if (pending.size() < 2) {
            if (pending.size() == 1 && pending.front()->files.size() >= 2) {
                result.groups.push_back(makeGroup(
                    pending, {}, "same_file_identity", bucket.logicalSize, 0));
                ++result.progress.verifiedGroups;
                emitProgress(onProgress, result.progress);
            }
            continue;
        }

        const bool useSample = bucket.logicalSize > kDuplicateSampleThresholdBytes;
        std::map<std::string, std::vector<IdentityWork*>> sampleGroups;
        for (IdentityWork* work : pending) {
            if (stopRequested(cancelled)) {
                result.cancelled = true;
                skipFiles(result, work->files, DuplicateFileStatus::Cancelled,
                          "Scan cancelled");
                continue;
            }
            if (!useSample) {
                sampleGroups["full"].push_back(work);
                continue;
            }
            const ContentHashResult hashed =
                hashIdentity(hasher, *work, bucket.logicalSize,
                             ContentHashKind::Sample, cancelled);
            result.progress.bytesRead += hashed.bytesRead;
            ++result.progress.filesFingerprinted;
            emitProgress(onProgress, result.progress);
            if (hashed.status != DuplicateFileStatus::Verified) {
                work->status = hashed.status;
                skipFiles(result, work->files, hashed.status, hashed.detail);
                continue;
            }
            work->sample = hashed.digest;
            work->hasSample = true;
            sampleGroups[sha256ToHex(hashed.digest)].push_back(work);
        }
        if (result.cancelled) {
            break;
        }

        for (auto& [_, sampleCluster] : sampleGroups) {
            if (sampleCluster.size() < 2) {
                if (sampleCluster.size() == 1 &&
                    sampleCluster.front()->files.size() >= 2) {
                    result.groups.push_back(makeGroup(sampleCluster, {},
                                                      "same_file_identity",
                                                      bucket.logicalSize, 0));
                    ++result.progress.verifiedGroups;
                }
                continue;
            }

            std::map<std::string, std::vector<IdentityWork*>> fullGroups;
            for (IdentityWork* work : sampleCluster) {
                if (stopRequested(cancelled)) {
                    result.cancelled = true;
                    skipFiles(result, work->files, DuplicateFileStatus::Cancelled,
                              "Scan cancelled");
                    continue;
                }
                const ContentHashResult hashed =
                    hashIdentity(hasher, *work, bucket.logicalSize,
                                 ContentHashKind::Full, cancelled);
                result.progress.bytesRead += hashed.bytesRead;
                ++result.progress.filesFullyHashed;
                emitProgress(onProgress, result.progress);
                if (hashed.status != DuplicateFileStatus::Verified) {
                    work->status = hashed.status;
                    skipFiles(result, work->files, hashed.status, hashed.detail);
                    continue;
                }
                work->full = hashed.digest;
                work->hasFull = true;
                fullGroups[sha256ToHex(hashed.digest)].push_back(work);
            }
            if (result.cancelled) {
                break;
            }

            for (auto& [hashHex, fullCluster] : fullGroups) {
                if (fullCluster.size() >= 2) {
                    result.groups.push_back(makeGroup(fullCluster, hashHex,
                                                      "full_sha256",
                                                      bucket.logicalSize, 0));
                    ++result.progress.verifiedGroups;
                } else if (fullCluster.size() == 1 &&
                           fullCluster.front()->files.size() >= 2) {
                    result.groups.push_back(makeGroup(fullCluster, hashHex,
                                                      "same_file_identity",
                                                      bucket.logicalSize, 0));
                    ++result.progress.verifiedGroups;
                }
            }
        }
        emitProgress(onProgress, result.progress);
    }

    if (result.cancelled) {
        result.completed = false;
    } else {
        result.completed = result.error.empty();
    }

    std::sort(result.skipped.begin(), result.skipped.end(),
              [](const DuplicateSkip& a, const DuplicateSkip& b) {
                  const auto left = normalizeCleanupPath(a.path);
                  const auto right = normalizeCleanupPath(b.path);
                  if (left != right) {
                      return left < right;
                  }
                  return a.path < b.path;
              });
    sortDuplicateGroups(result.groups);
    accumulateDuplicateSummary(result);
    return result;
}

}  // namespace spacelens
