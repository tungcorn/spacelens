#include "TestRunner.hpp"

#include "core/DuplicateDetection.hpp"
#include "core/Duplicates.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/index/IndexStore.hpp"
#include "core/index/Sqlite.hpp"
#include "platform/windows/CleanupMetadataReader.hpp"
#include "platform/windows/FileContentHasher.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace spacelens;
namespace fs = std::filesystem;

namespace {

fs::path uniqueTemp(const char* tag)
{
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = fs::temp_directory_path() / "spacelens_dup_tests" /
               (std::string(tag) + "_" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

std::wstring leafName(const std::wstring& path)
{
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

IndexLocation tempLocation(const fs::path& dir)
{
    IndexLocation loc;
    loc.rootPath = L"C:\\SpaceLensDupTestRoot";
    loc.rootKey = L"duptest";
    loc.indexDir = dir.wstring();
    loc.dbPath = (dir / "index.db").wstring();
    loc.stagingDbPath = (dir / "index.db.building").wstring();
    return loc;
}

IndexStore makeStore(const fs::path& dir)
{
    auto store = IndexStore::createStaging(tempLocation(dir));
    IndexRootInfo meta;
    meta.rootId = 1;
    meta.rootPath = L"C:\\SpaceLensDupTestRoot";
    meta.rootKey = L"duptest";
    meta.schemaVersion = kIndexSchemaVersion;
    meta.indexedAtIso = "2026-01-01T00:00:00Z";
    meta.status = IndexStatus::Ready;
    store.writeRootMeta(meta);
    return store;
}

void insertEntry(IndexStore& store,
                 const std::wstring& path,
                 ByteSize size,
                 int kind = 0,
                 int isReparse = 0)
{
    SqliteStmt stmt(
        store.db(),
        "INSERT INTO entries(root_id, kind, name, path, size_bytes, is_reparse) "
        "VALUES(1, ?1, ?2, ?3, ?4, ?5)");
    stmt.bindInt64(1, kind);
    stmt.bindText16(2, leafName(path));
    stmt.bindText16(3, path);
    stmt.bindInt64(4, static_cast<std::int64_t>(size));
    stmt.bindInt64(5, isReparse);
    stmt.stepDone();
}

CleanupIdentity strongId(std::uint8_t seed, std::uint64_t volume = 11)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = seed;
    bytes[15] = static_cast<std::uint8_t>(seed + 3);
    return makeFileId128Identity(volume, bytes);
}

CleanupMetadataProbe presentFile(const CleanupIdentity& identity,
                                 ByteSize size,
                                 FileTimeTicks writeTime = 50)
{
    CleanupMetadataProbe probe;
    probe.outcome = CleanupMetadataProbeOutcome::Present;
    probe.objectEvidence.available = true;
    probe.objectEvidence.identity = identity;
    probe.objectEvidence.kind = ItemKind::File;
    probe.objectEvidence.sizeScope = CleanupEvidenceScope::Direct;
    probe.objectEvidence.logicalSize = size;
    probe.objectEvidence.lastWriteTime = writeTime;
    probe.objectEvidence.attributes = 32;
    return probe;
}

class MapReader final : public ICleanupMetadataReader {
public:
    std::map<std::wstring, CleanupMetadataProbe> probes;

    CleanupMetadataProbe read(const std::wstring& path) override
    {
        const auto it = probes.find(path);
        if (it == probes.end()) {
            CleanupMetadataProbe missing;
            missing.outcome = CleanupMetadataProbeOutcome::Missing;
            missing.detail = "not in map";
            return missing;
        }
        return it->second;
    }
};

class MapHasher final : public IFileContentHasher {
public:
    struct Script {
        DuplicateFileStatus status = DuplicateFileStatus::Verified;
        std::array<std::uint8_t, 32> digest{};
        ByteSize bytesRead = 32;
        std::string detail;
        int* calls = nullptr;
    };

    std::map<std::wstring, Script> sample;
    std::map<std::wstring, Script> full;
    bool cancelAll = false;

    ContentHashResult hash(const ContentHashRequest& request) override
    {
        if (cancelAll || (request.cancelled && request.cancelled())) {
            ContentHashResult result;
            result.status = DuplicateFileStatus::Cancelled;
            result.detail = "cancelled";
            return result;
        }
        auto& table =
            request.kind == ContentHashKind::Sample ? sample : full;
        const auto it = table.find(request.path);
        ContentHashResult result;
        if (it == table.end()) {
            result.status = DuplicateFileStatus::ReadError;
            result.detail = "no script";
            return result;
        }
        if (it->second.calls != nullptr) {
            ++(*it->second.calls);
        }
        result.status = it->second.status;
        result.digest = it->second.digest;
        result.bytesRead = it->second.bytesRead;
        result.detail = it->second.detail;
        result.identity = request.expectedIdentity;
        result.logicalSize = request.expectedSize;
        result.lastWrite = request.expectedLastWrite;
        return result;
    }
};

std::array<std::uint8_t, 32> digest(std::uint8_t seed)
{
    std::array<std::uint8_t, 32> out{};
    out[0] = seed;
    out[31] = static_cast<std::uint8_t>(seed + 1);
    return out;
}

DuplicateIndexCandidate cand(std::wstring path, ByteSize size)
{
    DuplicateIndexCandidate file;
    file.path = std::move(path);
    file.name = leafName(file.path);
    file.logicalSize = size;
    return file;
}

void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> patternBytes(std::size_t size, std::uint8_t fill,
                                       std::size_t tweakAt = static_cast<std::size_t>(-1),
                                       std::uint8_t tweak = 0)
{
    std::vector<std::uint8_t> bytes(size, fill);
    if (tweakAt < bytes.size()) {
        bytes[tweakAt] = tweak;
    }
    return bytes;
}

}  // namespace

SPACELENS_TEST(Duplicates_redundant_bytes_two_and_three)
{
    SPACELENS_REQUIRE_EQ(redundantLogicalBytes(2, 4ULL * 1024ULL * 1024ULL * 1024ULL),
                         4ULL * 1024ULL * 1024ULL * 1024ULL);
    SPACELENS_REQUIRE_EQ(redundantLogicalBytes(3, 4ULL * 1024ULL * 1024ULL * 1024ULL),
                         8ULL * 1024ULL * 1024ULL * 1024ULL);
    SPACELENS_REQUIRE_EQ(redundantLogicalBytes(1, 100ULL * 1024ULL * 1024ULL), 0ULL);
    SPACELENS_REQUIRE_EQ(redundantLogicalBytes(0, 100), 0ULL);
}

SPACELENS_TEST(Duplicates_redundant_bytes_saturate)
{
    bool saturated = false;
    const ByteSize value = redundantLogicalBytes(
        3, std::numeric_limits<ByteSize>::max(), saturated);
    SPACELENS_REQUIRE(saturated);
    SPACELENS_REQUIRE_EQ(value, std::numeric_limits<ByteSize>::max());
}

SPACELENS_TEST(Duplicates_hardlink_only_group_has_zero_redundant)
{
    DuplicateGroup group;
    group.logicalSize = 100ULL * 1024ULL * 1024ULL;
    DuplicateContentInstance instance;
    instance.identity = strongId(1);
    instance.paths.push_back({L"C:\\a.bin", DuplicateFileStatus::SameIdentity});
    instance.paths.push_back({L"C:\\b.bin", DuplicateFileStatus::SameIdentity});
    group.instances.push_back(instance);
    finalizeDuplicateGroup(group);
    SPACELENS_REQUIRE_EQ(group.distinctIdentityCount, 1ULL);
    SPACELENS_REQUIRE_EQ(group.pathCount, 2ULL);
    SPACELENS_REQUIRE_EQ(group.hardLinkAliasPathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(group.redundantCopyCount, 0ULL);
    SPACELENS_REQUIRE_EQ(group.potentialRedundantLogicalBytes, 0ULL);
}

SPACELENS_TEST(Duplicates_mixed_alias_and_independent_copy)
{
    DuplicateGroup group;
    group.logicalSize = 100ULL * 1024ULL * 1024ULL;
    DuplicateContentInstance a;
    a.identity = strongId(1);
    a.paths.push_back({L"C:\\original.bin"});
    a.paths.push_back({L"C:\\alias.bin"});
    DuplicateContentInstance b;
    b.identity = strongId(2);
    b.paths.push_back({L"C:\\copy.bin"});
    group.instances.push_back(std::move(a));
    group.instances.push_back(std::move(b));
    finalizeDuplicateGroup(group);
    SPACELENS_REQUIRE_EQ(group.distinctIdentityCount, 2ULL);
    SPACELENS_REQUIRE_EQ(group.pathCount, 3ULL);
    SPACELENS_REQUIRE_EQ(group.hardLinkAliasPathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(group.redundantCopyCount, 1ULL);
    SPACELENS_REQUIRE_EQ(group.potentialRedundantLogicalBytes,
                         100ULL * 1024ULL * 1024ULL);
}

SPACELENS_TEST(Duplicates_group_order_is_deterministic)
{
    DuplicateGroup small;
    small.logicalSize = 10;
    small.potentialRedundantLogicalBytes = 10;
    small.contentSha256Hex = "bb";
    DuplicateContentInstance s;
    s.paths.push_back({L"C:\\b.bin"});
    small.instances.push_back(s);
    finalizeDuplicateGroup(small);

    DuplicateGroup large;
    large.logicalSize = 20;
    large.potentialRedundantLogicalBytes = 20;
    large.contentSha256Hex = "aa";
    DuplicateContentInstance l;
    l.paths.push_back({L"C:\\a.bin"});
    large.instances.push_back(l);
    finalizeDuplicateGroup(large);

    std::vector<DuplicateGroup> groups{small, large};
    sortDuplicateGroups(groups);
    SPACELENS_REQUIRE_EQ(groups.front().logicalSize, 20ULL);
}

SPACELENS_TEST(Duplicates_candidates_none_when_sizes_unique)
{
    const auto dir = uniqueTemp("unique");
    auto store = makeStore(dir);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\a.bin", 2 * 1024 * 1024);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\b.bin", 3 * 1024 * 1024);
    const auto result =
        queryDuplicateSizeCandidates(store, kDefaultDuplicateMinSize);
    SPACELENS_REQUIRE(result.ok);
    SPACELENS_REQUIRE_EQ(result.buckets.size(), 0ULL);
}

SPACELENS_TEST(Duplicates_candidates_two_same_size)
{
    const auto dir = uniqueTemp("pair");
    auto store = makeStore(dir);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\b.bin", 2 * 1024 * 1024);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\a.bin", 2 * 1024 * 1024);
    const auto result =
        queryDuplicateSizeCandidates(store, kDefaultDuplicateMinSize);
    SPACELENS_REQUIRE(result.ok);
    SPACELENS_REQUIRE_EQ(result.buckets.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(result.buckets.front().files.size(), 2ULL);
    SPACELENS_REQUIRE(result.buckets.front().files[0].path.find(L"a.bin") !=
                      std::wstring::npos);
    SPACELENS_REQUIRE(result.buckets.front().files[1].path.find(L"b.bin") !=
                      std::wstring::npos);
}

SPACELENS_TEST(Duplicates_candidates_several_groups_min_size_and_files_only)
{
    const auto dir = uniqueTemp("groups");
    auto store = makeStore(dir);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\tiny1.bin", 100);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\tiny2.bin", 100);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\mid1.bin", 2 * 1024 * 1024);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\mid2.bin", 2 * 1024 * 1024);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\big1.bin", 8 * 1024 * 1024);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\big2.bin", 8 * 1024 * 1024);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\folder", 8 * 1024 * 1024, 1);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\link.bin", 2 * 1024 * 1024, 0, 1);
    const auto result =
        queryDuplicateSizeCandidates(store, kDefaultDuplicateMinSize);
    SPACELENS_REQUIRE(result.ok);
    SPACELENS_REQUIRE_EQ(result.buckets.size(), 2ULL);
    SPACELENS_REQUIRE_EQ(result.buckets[0].logicalSize, 8ULL * 1024ULL * 1024ULL);
    SPACELENS_REQUIRE_EQ(result.buckets[1].logicalSize, 2ULL * 1024ULL * 1024ULL);
    SPACELENS_REQUIRE_EQ(result.candidateFiles, 4ULL);

    const auto tiny = queryDuplicateSizeCandidates(store, 0);
    SPACELENS_REQUIRE(tiny.ok);
    SPACELENS_REQUIRE_EQ(tiny.buckets.size(), 3ULL);
}

SPACELENS_TEST(Duplicates_candidates_unicode_path)
{
    const auto dir = uniqueTemp("unicode");
    auto store = makeStore(dir);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\unicodé-文件.bin",
                2 * 1024 * 1024);
    insertEntry(store, L"C:\\SpaceLensDupTestRoot\\other.bin", 2 * 1024 * 1024);
    const auto result =
        queryDuplicateSizeCandidates(store, kDefaultDuplicateMinSize);
    SPACELENS_REQUIRE(result.ok);
    SPACELENS_REQUIRE_EQ(result.buckets.front().files.size(), 2ULL);
}

SPACELENS_TEST(Duplicates_same_size_is_not_verified)
{
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 100;
    bucket.files.push_back(cand(L"C:\\a.bin", 100));
    bucket.files.push_back(cand(L"C:\\b.bin", 100));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;

    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(strongId(1), 100);
    reader.probes[L"C:\\b.bin"] = presentFile(strongId(2), 100);

    MapHasher hasher;
    hasher.full[L"C:\\a.bin"] = {DuplicateFileStatus::Verified, digest(1)};
    hasher.full[L"C:\\b.bin"] = {DuplicateFileStatus::Verified, digest(2)};

    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 0ULL);
    const auto json = result.toJson();
    SPACELENS_REQUIRE(json.find("\"verified_groups\":0") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"filesystem_mutation\":false") !=
                      std::string::npos);
}

SPACELENS_TEST(Duplicates_sample_match_is_not_verified)
{
    const ByteSize size = kDuplicateSampleThresholdBytes + 10;
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = size;
    bucket.files.push_back(cand(L"C:\\a.bin", size));
    bucket.files.push_back(cand(L"C:\\b.bin", size));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;

    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(strongId(1), size);
    reader.probes[L"C:\\b.bin"] = presentFile(strongId(2), size);

    MapHasher hasher;
    hasher.sample[L"C:\\a.bin"] = {DuplicateFileStatus::Verified, digest(9)};
    hasher.sample[L"C:\\b.bin"] = {DuplicateFileStatus::Verified, digest(9)};
    hasher.full[L"C:\\a.bin"] = {DuplicateFileStatus::Verified, digest(1)};
    hasher.full[L"C:\\b.bin"] = {DuplicateFileStatus::Verified, digest(2)};

    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 0ULL);
    SPACELENS_REQUIRE(result.progress.filesFingerprinted >= 2);
    SPACELENS_REQUIRE(result.progress.filesFullyHashed >= 2);
}

SPACELENS_TEST(Duplicates_full_hash_verified_two_independent)
{
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 100;
    bucket.files.push_back(cand(L"C:\\a.bin", 100));
    bucket.files.push_back(cand(L"C:\\b.bin", 100));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;

    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(strongId(1), 100);
    reader.probes[L"C:\\b.bin"] = presentFile(strongId(2), 100);
    MapHasher hasher;
    hasher.full[L"C:\\a.bin"] = {DuplicateFileStatus::Verified, digest(4)};
    hasher.full[L"C:\\b.bin"] = {DuplicateFileStatus::Verified, digest(4)};

    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().distinctIdentityCount, 2ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().potentialRedundantLogicalBytes, 100ULL);
    SPACELENS_REQUIRE(result.groups.front().verification == "full_sha256");
    SPACELENS_REQUIRE(result.groups.front().contentSha256Hex == sha256ToHex(digest(4)));
}

SPACELENS_TEST(Duplicates_hardlinks_collapsed_before_hash)
{
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 100;
    bucket.files.push_back(cand(L"C:\\original.bin", 100));
    bucket.files.push_back(cand(L"C:\\alias.bin", 100));
    bucket.files.push_back(cand(L"C:\\copy.bin", 100));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 3;

    const auto shared = strongId(1);
    MapReader reader;
    reader.probes[L"C:\\original.bin"] = presentFile(shared, 100);
    reader.probes[L"C:\\alias.bin"] = presentFile(shared, 100);
    reader.probes[L"C:\\copy.bin"] = presentFile(strongId(2), 100);
    int hashCalls = 0;
    MapHasher hasher;
    MapHasher::Script script{DuplicateFileStatus::Verified, digest(7), 100, {},
                             &hashCalls};
    hasher.full[L"C:\\alias.bin"] = script;
    hasher.full[L"C:\\copy.bin"] = script;
    hasher.full[L"C:\\original.bin"] = script;

    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().pathCount, 3ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().distinctIdentityCount, 2ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().hardLinkAliasPathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().potentialRedundantLogicalBytes, 100ULL);
    SPACELENS_REQUIRE_EQ(static_cast<std::uint64_t>(hashCalls), 2ULL);
}

SPACELENS_TEST(Duplicates_hardlink_only_group_skips_hash)
{
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 100;
    bucket.files.push_back(cand(L"C:\\a.bin", 100));
    bucket.files.push_back(cand(L"C:\\b.bin", 100));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;

    const auto shared = strongId(9);
    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(shared, 100);
    reader.probes[L"C:\\b.bin"] = presentFile(shared, 100);
    int hashCalls = 0;
    MapHasher hasher;
    hasher.full[L"C:\\a.bin"] = {DuplicateFileStatus::Verified, digest(1), 100, {},
                                 &hashCalls};

    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 1ULL);
    SPACELENS_REQUIRE(result.groups.front().verification == "same_file_identity");
    SPACELENS_REQUIRE_EQ(result.groups.front().potentialRedundantLogicalBytes, 0ULL);
    SPACELENS_REQUIRE_EQ(hashCalls, 0);
}

SPACELENS_TEST(Duplicates_stale_missing_and_reparse_are_skipped)
{
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 100;
    bucket.files.push_back(cand(L"C:\\gone.bin", 100));
    bucket.files.push_back(cand(L"C:\\link.bin", 100));
    bucket.files.push_back(cand(L"C:\\ok.bin", 100));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 3;

    MapReader reader;
    CleanupMetadataProbe reparse = presentFile(strongId(3), 100);
    reparse.isReparse = true;
    reader.probes[L"C:\\link.bin"] = reparse;
    reader.probes[L"C:\\ok.bin"] = presentFile(strongId(4), 100);

    MapHasher hasher;
    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 0ULL);
    SPACELENS_REQUIRE_EQ(result.skipped.size(), 2ULL);
    bool sawMissing = false;
    bool sawReparse = false;
    for (const auto& skip : result.skipped) {
        if (skip.status == DuplicateFileStatus::Missing) {
            sawMissing = true;
        }
        if (skip.status == DuplicateFileStatus::ReparsePoint) {
            sawReparse = true;
        }
    }
    SPACELENS_REQUIRE(sawMissing);
    SPACELENS_REQUIRE(sawReparse);
}

SPACELENS_TEST(Duplicates_changed_during_read_rejected)
{
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 100;
    bucket.files.push_back(cand(L"C:\\a.bin", 100));
    bucket.files.push_back(cand(L"C:\\b.bin", 100));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;

    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(strongId(1), 100);
    reader.probes[L"C:\\b.bin"] = presentFile(strongId(2), 100);
    MapHasher hasher;
    hasher.full[L"C:\\a.bin"] = {DuplicateFileStatus::ChangedDuringRead, digest(1),
                                 20, "changed"};
    hasher.full[L"C:\\b.bin"] = {DuplicateFileStatus::Verified, digest(1)};

    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 0ULL);
    bool sawChanged = false;
    for (const auto& skip : result.skipped) {
        if (skip.status == DuplicateFileStatus::ChangedDuringRead) {
            sawChanged = true;
        }
    }
    SPACELENS_REQUIRE(sawChanged);
}

SPACELENS_TEST(Duplicates_identity_changed_rejected)
{
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 100;
    bucket.files.push_back(cand(L"C:\\a.bin", 100));
    bucket.files.push_back(cand(L"C:\\b.bin", 100));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;

    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(strongId(1), 100);
    reader.probes[L"C:\\b.bin"] = presentFile(strongId(2), 100);
    MapHasher hasher;
    hasher.full[L"C:\\a.bin"] = {DuplicateFileStatus::IdentityChanged, {}, 0,
                                 "replaced"};
    hasher.full[L"C:\\b.bin"] = {DuplicateFileStatus::Verified, digest(1)};

    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 0ULL);
}

SPACELENS_TEST(Duplicates_cancel_marks_partial)
{
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 100;
    bucket.files.push_back(cand(L"C:\\a.bin", 100));
    bucket.files.push_back(cand(L"C:\\b.bin", 100));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;

    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(strongId(1), 100);
    reader.probes[L"C:\\b.bin"] = presentFile(strongId(2), 100);
    MapHasher hasher;
    hasher.cancelAll = true;
    hasher.full[L"C:\\a.bin"] = {DuplicateFileStatus::Verified, digest(1)};
    hasher.full[L"C:\\b.bin"] = {DuplicateFileStatus::Verified, digest(1)};

    bool stop = true;
    const auto result = detectDuplicates(candidates, reader, hasher, {},
                                         [&stop]() { return stop; });
    SPACELENS_REQUIRE(result.cancelled);
    SPACELENS_REQUIRE(!result.completed);
}

SPACELENS_TEST(Duplicates_json_does_not_export_candidates_as_groups)
{
    DuplicateDetectionResult result;
    result.completed = true;
    result.progress.candidateFiles = 4;
    accumulateDuplicateSummary(result);
    const auto json = result.toJson();
    SPACELENS_REQUIRE(json.find("\"groups\":[]") != std::string::npos);
    SPACELENS_REQUIRE(json.find("planning_only") != std::string::npos);
}

SPACELENS_TEST(Duplicates_live_hasher_identical_and_different)
{
    const auto dir = uniqueTemp("livehash");
    const auto a = dir / "a.bin";
    const auto b = dir / "b.bin";
    const auto c = dir / "c.bin";
    writeBytes(a, patternBytes(4096, 'A'));
    writeBytes(b, patternBytes(4096, 'A'));
    writeBytes(c, patternBytes(4096, 'B'));

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 4096;
    bucket.files.push_back(cand(a.wstring(), 4096));
    bucket.files.push_back(cand(b.wstring(), 4096));
    bucket.files.push_back(cand(c.wstring(), 4096));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 3;

    const auto result = detectDuplicates(candidates, reader, hasher, {});
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().pathCount, 2ULL);
    SPACELENS_REQUIRE(result.groups.front().verification == "full_sha256");
    SPACELENS_REQUIRE(!result.groups.front().contentSha256Hex.empty());
}

SPACELENS_TEST(Duplicates_live_hasher_middle_difference)
{
    const auto dir = uniqueTemp("middle");
    const ByteSize size = 256ULL * 1024ULL;
    const auto a = dir / "a.bin";
    const auto b = dir / "b.bin";
    writeBytes(a, patternBytes(static_cast<std::size_t>(size), 'X'));
    writeBytes(b, patternBytes(static_cast<std::size_t>(size), 'X', 80ULL * 1024ULL,
                               'Y'));

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = size;
    bucket.files.push_back(cand(a.wstring(), size));
    bucket.files.push_back(cand(b.wstring(), size));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;

    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 0ULL);
}

SPACELENS_TEST(Duplicates_live_hasher_unicode_and_multimeg)
{
    const auto dir = uniqueTemp("biguni");
    const ByteSize size = 2ULL * 1024ULL * 1024ULL;
    const auto a = dir / L"unicodé-文件.bin";
    const auto b = dir / L"copy.bin";
    writeBytes(a, patternBytes(static_cast<std::size_t>(size), 0x5A));
    writeBytes(b, patternBytes(static_cast<std::size_t>(size), 0x5A));

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = size;
    bucket.files.push_back(cand(a.wstring(), size));
    bucket.files.push_back(cand(b.wstring(), size));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;

    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().logicalSize, size);
}

SPACELENS_TEST(Duplicates_zero_length_ignored)
{
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 0;
    bucket.files.push_back(cand(L"C:\\empty1", 0));
    bucket.files.push_back(cand(L"C:\\empty2", 0));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;
    MapReader reader;
    MapHasher hasher;
    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 0ULL);
}

SPACELENS_TEST(Duplicates_windows_hardlink_fixture)
{
    const auto dir = uniqueTemp("hardlink");
    const ByteSize size = 1024ULL * 1024ULL;
    const auto original = dir / "original.bin";
    const auto alias = dir / "alias.bin";
    const auto copy = dir / "copy.bin";
    writeBytes(original, patternBytes(static_cast<std::size_t>(size), 0x11));
    writeBytes(copy, patternBytes(static_cast<std::size_t>(size), 0x11));
    if (!::CreateHardLinkW(alias.wstring().c_str(), original.wstring().c_str(),
                           nullptr)) {
        std::cout << "[ SKIP ] Duplicates_windows_hardlink_fixture — "
                  << "CreateHardLinkW failed, err=" << ::GetLastError() << "\n";
        return;
    }

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = size;
    bucket.files.push_back(cand(original.wstring(), size));
    bucket.files.push_back(cand(alias.wstring(), size));
    bucket.files.push_back(cand(copy.wstring(), size));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 3;

    const auto result = detectDuplicates(candidates, reader, hasher);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.groups.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().pathCount, 3ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().distinctIdentityCount, 2ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().hardLinkAliasPathCount, 1ULL);
    SPACELENS_REQUIRE_EQ(result.groups.front().potentialRedundantLogicalBytes, size);
}

SPACELENS_TEST(Duplicates_add_to_review_keeps_provenance)
{
    DuplicateGroup group;
    group.contentSha256Hex = "abcd";
    group.verification = "full_sha256";
    group.logicalSize = 10;
    DuplicateContentInstance instance;
    instance.identity = strongId(1);
    DuplicatePathRecord path;
    path.path = L"C:\\dup.bin";
    path.lastWrite = 9;
    instance.paths.push_back(path);
    group.instances.push_back(instance);
    finalizeDuplicateGroup(group);

    const auto candidate = cleanupCandidateFromDuplicate(
        group, group.instances.front(), group.instances.front().paths.front(),
        L"C:\\", 12, "2026-01-01T00:00:00Z");
    SPACELENS_REQUIRE(candidate.source == "duplicate_detection");
    SPACELENS_REQUIRE(candidate.reasonAdded.find("sha256=abcd") != std::string::npos);
    SPACELENS_REQUIRE_EQ(candidate.indexAgeMs, 12ULL);
    SPACELENS_REQUIRE(candidate.kind == ItemKind::File);
}
