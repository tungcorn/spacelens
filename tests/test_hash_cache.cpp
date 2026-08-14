#include "TestRunner.hpp"

#include "core/DuplicateDetection.hpp"
#include "core/Duplicates.hpp"
#include "core/HashCache.hpp"
#include "core/index/IndexPaths.hpp"
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
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace spacelens;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    explicit TempDir(const char* tag)
    {
        path = fs::temp_directory_path() / "spacelens_hash_cache_tests" /
               (std::string(tag) + "_" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    [[nodiscard]] std::wstring dbPath() const
    {
        return (path / "hash-cache.db").wstring();
    }
};

CleanupIdentity strongId(std::uint8_t seed, std::uint64_t volume = 42)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = seed;
    bytes[15] = static_cast<std::uint8_t>(seed + 9);
    return makeFileId128Identity(volume, bytes);
}

ContentHashEvidence persistableEvidence(std::uint8_t seed,
                                        ByteSize size = 1000,
                                        FileTimeTicks change = 100,
                                        std::int64_t usn = 7)
{
    ContentHashEvidence evidence;
    evidence.identity = strongId(seed);
    evidence.logicalSize = size;
    evidence.lastWrite = change;
    evidence.changeTime = change;
    evidence.fileUsn = usn;
    evidence.journalId = 0;
    evidence.persistable = isHashCachePersistable(evidence);
    evidence.status = DuplicateFileStatus::Verified;
    return evidence;
}

std::array<std::uint8_t, 32> digestOf(std::uint8_t seed)
{
    std::array<std::uint8_t, 32> out{};
    out[0] = seed;
    out[31] = static_cast<std::uint8_t>(seed + 1);
    return out;
}

HashCacheRow rowFrom(const ContentHashEvidence& evidence,
                     const std::array<std::uint8_t, 32>& digest)
{
    HashCacheRow row;
    row.identity = evidence.identity;
    row.logicalSize = evidence.logicalSize;
    row.changeTime = evidence.changeTime;
    row.fileUsn = evidence.fileUsn;
    row.journalId = evidence.journalId;
    row.algorithm = kHashAlgorithmSha256;
    row.evidenceVersion = kHashEvidenceVersion;
    row.digest.assign(digest.begin(), digest.end());
    return row;
}

std::wstring leafName(const std::wstring& path)
{
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

DuplicateIndexCandidate cand(std::wstring path, ByteSize size)
{
    DuplicateIndexCandidate file;
    file.path = std::move(path);
    file.name = leafName(file.path);
    file.logicalSize = size;
    return file;
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

class ScriptHasher final : public IFileContentHasher {
public:
    ContentHashEvidence probeEvidence{};
    ContentHashResult fullResult{};
    int hashCalls = 0;
    int probeCalls = 0;
    DuplicateFileStatus hashStatus = DuplicateFileStatus::Verified;

    ContentHashEvidence probe(const std::wstring&) override
    {
        ++probeCalls;
        return probeEvidence;
    }

    ContentHashResult hash(const ContentHashRequest& request) override
    {
        ++hashCalls;
        if (request.kind == ContentHashKind::Sample) {
            ContentHashResult sample = fullResult;
            sample.status = DuplicateFileStatus::Verified;
            return sample;
        }
        ContentHashResult result = fullResult;
        result.status = hashStatus;
        return result;
    }
};

void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> patternBytes(std::size_t size, std::uint8_t fill)
{
    return std::vector<std::uint8_t>(size, fill);
}

DuplicateCandidateQueryResult twoFileCandidates(const fs::path& a,
                                                const fs::path& b,
                                                ByteSize size)
{
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = size;
    bucket.files.push_back(cand(a.wstring(), size));
    bucket.files.push_back(cand(b.wstring(), size));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 2;
    candidates.candidateBytes = size * 2;
    return candidates;
}

bool productCacheUntouched(const WIN32_FILE_ATTRIBUTE_DATA* before,
                           bool existedBefore)
{
    const std::wstring product = spaceLensHashCachePath();
    WIN32_FILE_ATTRIBUTE_DATA after{};
    const bool existsNow =
        ::GetFileAttributesExW(product.c_str(), GetFileExInfoStandard, &after) !=
        0;
    if (!existedBefore) {
        return !existsNow;
    }
    return existsNow &&
           after.nFileSizeLow == before->nFileSizeLow &&
           after.nFileSizeHigh == before->nFileSizeHigh &&
           after.ftLastWriteTime.dwLowDateTime ==
               before->ftLastWriteTime.dwLowDateTime &&
           after.ftLastWriteTime.dwHighDateTime ==
               before->ftLastWriteTime.dwHighDateTime;
}

}  // namespace

SPACELENS_TEST(HashCache_empty_path_disables)
{
    auto store = HashCacheStore::tryOpen(L"");
    SPACELENS_REQUIRE(!store.available());
    const auto looked = store.lookup(persistableEvidence(1));
    SPACELENS_REQUIRE_EQ(looked.disposition, HashCacheDisposition::Disabled);
}

SPACELENS_TEST(HashCache_evaluate_reusable_and_mismatches)
{
    const auto live = persistableEvidence(1, 2048, 500, 11);
    const auto digest = digestOf(9);
    SPACELENS_REQUIRE_EQ(evaluateHashCacheRow(live, rowFrom(live, digest)),
                         HashCacheDisposition::Reusable);

    auto sizeChange = live;
    sizeChange.logicalSize = 4096;
    SPACELENS_REQUIRE_EQ(
        evaluateHashCacheRow(sizeChange, rowFrom(live, digest)),
        HashCacheDisposition::MustRehash);

    auto timeChange = live;
    timeChange.changeTime = 999;
    SPACELENS_REQUIRE_EQ(
        evaluateHashCacheRow(timeChange, rowFrom(live, digest)),
        HashCacheDisposition::MustRehash);

    auto usnChange = live;
    usnChange.fileUsn = 99;
    SPACELENS_REQUIRE_EQ(evaluateHashCacheRow(usnChange, rowFrom(live, digest)),
                         HashCacheDisposition::MustRehash);
}

SPACELENS_TEST(HashCache_evaluate_invalid_row)
{
    const auto live = persistableEvidence(1);
    auto bad = rowFrom(live, digestOf(1));
    bad.digest.resize(4);
    SPACELENS_REQUIRE_EQ(evaluateHashCacheRow(live, bad),
                         HashCacheDisposition::Invalid);

    bad = rowFrom(live, digestOf(1));
    bad.algorithm = 99;
    SPACELENS_REQUIRE_EQ(evaluateHashCacheRow(live, bad),
                         HashCacheDisposition::Invalid);

    bad = rowFrom(live, digestOf(1));
    bad.evidenceVersion = 2;
    SPACELENS_REQUIRE_EQ(evaluateHashCacheRow(live, bad),
                         HashCacheDisposition::Invalid);

    bad = rowFrom(live, digestOf(1));
    bad.identity = makeFileIndex64FallbackIdentity(1, 99);
    SPACELENS_REQUIRE_EQ(evaluateHashCacheRow(live, bad),
                         HashCacheDisposition::Invalid);
}

SPACELENS_TEST(HashCache_fileindex64_not_persistable)
{
    ContentHashEvidence evidence;
    evidence.identity = makeFileIndex64FallbackIdentity(1, 77);
    evidence.logicalSize = 10;
    evidence.changeTime = 1;
    evidence.fileUsn = 1;
    SPACELENS_REQUIRE(!isHashCachePersistable(evidence));
}

SPACELENS_TEST(HashCache_missing_usn_or_changetime_not_persistable)
{
    auto evidence = persistableEvidence(1);
    evidence.fileUsn = 0;
    SPACELENS_REQUIRE(!isHashCachePersistable(evidence));
    evidence = persistableEvidence(1);
    evidence.changeTime = 0;
    SPACELENS_REQUIRE(!isHashCachePersistable(evidence));
}

SPACELENS_TEST(HashCache_journal_mismatch_only_when_both_set)
{
    auto live = persistableEvidence(1);
    live.journalId = 10;
    auto stored = rowFrom(live, digestOf(1));
    stored.journalId = 11;
    SPACELENS_REQUIRE_EQ(evaluateHashCacheRow(live, stored),
                         HashCacheDisposition::MustRehash);

    live.journalId = 0;
    stored.journalId = 11;
    SPACELENS_REQUIRE_EQ(evaluateHashCacheRow(live, stored),
                         HashCacheDisposition::Reusable);

    live.journalId = 11;
    stored.journalId = 0;
    SPACELENS_REQUIRE_EQ(evaluateHashCacheRow(live, stored),
                         HashCacheDisposition::Reusable);
}

SPACELENS_TEST(HashCache_store_lookup_roundtrip)
{
    const TempDir dir("roundtrip");
    auto store = HashCacheStore::tryOpen(dir.dbPath());
    SPACELENS_REQUIRE(store.available());
    const auto live = persistableEvidence(3, 1234, 88, 15);
    const auto digest = digestOf(3);
    SPACELENS_REQUIRE(store.store(live, digest));
    const auto looked = store.lookup(live);
    SPACELENS_REQUIRE_EQ(looked.disposition, HashCacheDisposition::Reusable);
    SPACELENS_REQUIRE(looked.digest == digest);
}

SPACELENS_TEST(HashCache_path_is_not_a_column)
{
    const TempDir dir("nopath");
    auto store = HashCacheStore::tryOpen(dir.dbPath());
    SPACELENS_REQUIRE(store.available());
    SqliteDb db(dir.dbPath(), SqliteOpen::ReadOnly);
    SqliteStmt stmt(db, "PRAGMA table_info(content_hashes)");
    while (stmt.step()) {
        const auto name = stmt.columnText(1);
        SPACELENS_REQUIRE(name != "path");
        SPACELENS_REQUIRE(name.find("path") == std::string::npos);
    }
}

SPACELENS_TEST(HashCache_same_identity_different_paths_share_row)
{
    const TempDir dir("identity");
    auto store = HashCacheStore::tryOpen(dir.dbPath());
    auto fromA = persistableEvidence(4);
    auto fromB = fromA;
    const auto digest = digestOf(4);
    SPACELENS_REQUIRE(store.store(fromA, digest));
    const auto looked = store.lookup(fromB);
    SPACELENS_REQUIRE_EQ(looked.disposition, HashCacheDisposition::Reusable);
    SPACELENS_REQUIRE(looked.digest == digest);
}

SPACELENS_TEST(HashCache_corrupt_row_invalidated)
{
    const TempDir dir("corrupt");
    {
        auto store = HashCacheStore::tryOpen(dir.dbPath());
        SPACELENS_REQUIRE(store.store(persistableEvidence(5), digestOf(5)));
    }
    {
        SqliteDb db(dir.dbPath(), SqliteOpen::ReadWrite);
        SqliteStmt stmt(db,
                        "UPDATE content_hashes SET digest = X'00FF'");
        stmt.stepDone();
    }
    auto store = HashCacheStore::tryOpen(dir.dbPath());
    const auto looked = store.lookup(persistableEvidence(5));
    SPACELENS_REQUIRE_EQ(looked.disposition, HashCacheDisposition::Invalid);
    const auto again = store.lookup(persistableEvidence(5));
    SPACELENS_REQUIRE_EQ(again.disposition, HashCacheDisposition::MustRehash);
}

SPACELENS_TEST(HashCache_unknown_schema_version_unavailable)
{
    const TempDir dir("newer");
    {
        auto store = HashCacheStore::tryOpen(dir.dbPath());
        SPACELENS_REQUIRE(store.available());
    }
    {
        SqliteDb db(dir.dbPath(), SqliteOpen::ReadWrite);
        SqliteStmt stmt(db,
                        "UPDATE meta SET value = '99' WHERE key = "
                        "'hash_cache_schema_version'");
        stmt.stepDone();
    }
    auto store = HashCacheStore::tryOpen(dir.dbPath());
    SPACELENS_REQUIRE(!store.available());
}

SPACELENS_TEST(HashCache_v1_reopen_still_works)
{
    const TempDir dir("migrate");
    const auto live = persistableEvidence(6);
    const auto digest = digestOf(6);
    {
        auto store = HashCacheStore::tryOpen(dir.dbPath());
        SPACELENS_REQUIRE(store.store(live, digest));
    }
    auto store = HashCacheStore::tryOpen(dir.dbPath());
    SPACELENS_REQUIRE(store.available());
    const auto looked = store.lookup(live);
    SPACELENS_REQUIRE_EQ(looked.disposition, HashCacheDisposition::Reusable);
}

SPACELENS_TEST(HashCache_write_failure_does_not_throw)
{
    auto store = HashCacheStore::tryOpen(L"\\\\?\\NUL\\spacelens-not-a-db");
    SPACELENS_REQUIRE(!store.available());
    SPACELENS_REQUIRE(!store.store(persistableEvidence(1), digestOf(1)));
}

SPACELENS_TEST(HashCache_detect_cold_warm_with_script_hasher)
{
    const TempDir dir("script");
    const auto idA = strongId(1);
    const auto idB = strongId(2);
    const ByteSize size = 100;
    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(idA, size);
    reader.probes[L"C:\\b.bin"] = presentFile(idB, size);

    ScriptHasher hasher;
    hasher.probeEvidence = persistableEvidence(1, size, 50, 8);
    hasher.probeEvidence.identity = idA;
    hasher.fullResult.status = DuplicateFileStatus::Verified;
    hasher.fullResult.digest = digestOf(20);
    hasher.fullResult.identity = idA;
    hasher.fullResult.logicalSize = size;
    hasher.fullResult.changeTime = 50;
    hasher.fullResult.fileUsn = 8;
    hasher.fullResult.persistable = true;

    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = size;
    bucket.files.push_back(cand(L"C:\\a.bin", size));
    bucket.files.push_back(cand(L"C:\\b.bin", size));
    candidates.buckets.push_back(std::move(bucket));
    candidates.candidateFiles = 2;

    DuplicateScanOptions options;
    options.hashCachePath = dir.dbPath();
    options.minimumSize = 1;

    // First identity hashes and writes; second identity is a different FileId
    // so it misses and hashes too. Warm scan of the same pair should hit.
    auto first = detectDuplicates(candidates, reader, hasher, options);
    SPACELENS_REQUIRE(first.completed);
    SPACELENS_REQUIRE(first.summary.cacheWrites >= 1);
    const int afterCold = hasher.hashCalls;

    ScriptHasher hasher2;
    hasher2.probeEvidence = hasher.probeEvidence;
    hasher2.fullResult = hasher.fullResult;
    auto second = detectDuplicates(candidates, reader, hasher2, options);
    SPACELENS_REQUIRE(second.completed);
    SPACELENS_REQUIRE(second.summary.cacheHits >= 1);
    SPACELENS_REQUIRE(hasher2.hashCalls < afterCold);
    SPACELENS_REQUIRE(first.groups.size() == second.groups.size());
}

SPACELENS_TEST(HashCache_cancelled_hash_not_persisted)
{
    const TempDir dir("cancel");
    const auto idA = strongId(1);
    const auto idB = strongId(2);
    const ByteSize size = 100;
    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(idA, size);
    reader.probes[L"C:\\b.bin"] = presentFile(idB, size);

    ScriptHasher hasher;
    hasher.probeEvidence = persistableEvidence(1, size, 50, 8);
    hasher.probeEvidence.identity = idA;
    hasher.hashStatus = DuplicateFileStatus::Cancelled;
    hasher.fullResult.identity = idA;
    hasher.fullResult.logicalSize = size;
    hasher.fullResult.changeTime = 50;
    hasher.fullResult.fileUsn = 8;
    hasher.fullResult.persistable = true;
    hasher.fullResult.digest = digestOf(1);

    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = size;
    bucket.files.push_back(cand(L"C:\\a.bin", size));
    bucket.files.push_back(cand(L"C:\\b.bin", size));
    candidates.buckets.push_back(std::move(bucket));
    candidates.candidateFiles = 2;

    DuplicateScanOptions options;
    options.hashCachePath = dir.dbPath();
    options.minimumSize = 1;
    const auto result = detectDuplicates(candidates, reader, hasher, options);
    SPACELENS_REQUIRE_EQ(result.summary.cacheWrites, 0ULL);

    auto store = HashCacheStore::tryOpen(dir.dbPath());
    const auto looked = store.lookup(hasher.probeEvidence);
    SPACELENS_REQUIRE_EQ(looked.disposition, HashCacheDisposition::MustRehash);
}

SPACELENS_TEST(HashCache_changed_during_read_not_persisted)
{
    const TempDir dir("changed");
    const auto idA = strongId(1);
    const auto idB = strongId(2);
    const ByteSize size = 100;
    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(idA, size);
    reader.probes[L"C:\\b.bin"] = presentFile(idB, size);

    ScriptHasher hasher;
    hasher.probeEvidence = persistableEvidence(1, size, 50, 8);
    hasher.probeEvidence.identity = idA;
    hasher.hashStatus = DuplicateFileStatus::ChangedDuringRead;
    hasher.fullResult.identity = idA;
    hasher.fullResult.logicalSize = size;
    hasher.fullResult.changeTime = 50;
    hasher.fullResult.fileUsn = 8;
    hasher.fullResult.persistable = true;
    hasher.fullResult.digest = digestOf(1);

    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = size;
    bucket.files.push_back(cand(L"C:\\a.bin", size));
    bucket.files.push_back(cand(L"C:\\b.bin", size));
    candidates.buckets.push_back(std::move(bucket));
    candidates.candidateFiles = 2;
    DuplicateScanOptions options;
    options.hashCachePath = dir.dbPath();
    options.minimumSize = 1;
    const auto result = detectDuplicates(candidates, reader, hasher, options);
    SPACELENS_REQUIRE_EQ(result.summary.cacheWrites, 0ULL);
}

SPACELENS_TEST(HashCache_default_options_do_not_touch_appdata)
{
    WIN32_FILE_ATTRIBUTE_DATA before{};
    const bool existed =
        ::GetFileAttributesExW(spaceLensHashCachePath().c_str(),
                               GetFileExInfoStandard, &before) != 0;

    const auto idA = strongId(1);
    const auto idB = strongId(2);
    MapReader reader;
    reader.probes[L"C:\\a.bin"] = presentFile(idA, 50);
    reader.probes[L"C:\\b.bin"] = presentFile(idB, 50);
    ScriptHasher hasher;
    hasher.fullResult.status = DuplicateFileStatus::Verified;
    hasher.fullResult.digest = digestOf(1);
    hasher.fullResult.identity = idA;
    hasher.fullResult.logicalSize = 50;

    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = 50;
    bucket.files.push_back(cand(L"C:\\a.bin", 50));
    bucket.files.push_back(cand(L"C:\\b.bin", 50));
    candidates.buckets.push_back(std::move(bucket));
    candidates.candidateFiles = 2;

    DuplicateScanOptions options;
    SPACELENS_REQUIRE(options.hashCachePath.empty());
    const auto result = detectDuplicates(candidates, reader, hasher, options);
    SPACELENS_REQUIRE(result.completed);
    SPACELENS_REQUIRE_EQ(result.summary.cacheHits, 0ULL);
    SPACELENS_REQUIRE_EQ(result.summary.cacheWrites, 0ULL);
    SPACELENS_REQUIRE(productCacheUntouched(&before, existed));
}

SPACELENS_TEST(HashCache_live_cold_warm_same_digest)
{
    const TempDir dir("livewarm");
    const ByteSize size = 256ULL * 1024ULL;
    const auto a = dir.path / "a.bin";
    const auto b = dir.path / "b.bin";
    writeBytes(a, patternBytes(static_cast<std::size_t>(size), 0x3C));
    writeBytes(b, patternBytes(static_cast<std::size_t>(size), 0x3C));

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    DuplicateScanOptions options;
    options.hashCachePath = dir.dbPath();
    options.minimumSize = 1;
    const auto candidates = twoFileCandidates(a, b, size);

    const auto cold = detectDuplicates(candidates, reader, hasher, options);
    SPACELENS_REQUIRE(cold.completed);
    SPACELENS_REQUIRE_EQ(cold.groups.size(), 1ULL);
    SPACELENS_REQUIRE(cold.summary.cacheWrites >= 1);
    SPACELENS_REQUIRE(cold.summary.filesFullyHashed >= 1);
    const auto hex = cold.groups.front().contentSha256Hex;

    WindowsFileContentHasher hasher2;
    const auto warm = detectDuplicates(candidates, reader, hasher2, options);
    SPACELENS_REQUIRE(warm.completed);
    SPACELENS_REQUIRE_EQ(warm.groups.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(warm.groups.front().contentSha256Hex, hex);
    SPACELENS_REQUIRE(warm.summary.cacheHits >= 1);
    SPACELENS_REQUIRE(warm.summary.filesFullyHashed < cold.summary.filesFullyHashed);
}

SPACELENS_TEST(HashCache_live_same_size_content_change_misses)
{
    const TempDir dir("change");
    const ByteSize size = 128ULL * 1024ULL;
    const auto a = dir.path / "a.bin";
    const auto b = dir.path / "b.bin";
    writeBytes(a, patternBytes(static_cast<std::size_t>(size), 0x11));
    writeBytes(b, patternBytes(static_cast<std::size_t>(size), 0x11));

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    DuplicateScanOptions options;
    options.hashCachePath = dir.dbPath();
    options.minimumSize = 1;
    auto candidates = twoFileCandidates(a, b, size);
    const auto cold = detectDuplicates(candidates, reader, hasher, options);
    SPACELENS_REQUIRE(cold.completed);
    const auto beforeHex = cold.groups.front().contentSha256Hex;

    writeBytes(a, patternBytes(static_cast<std::size_t>(size), 0x22));
    WindowsFileContentHasher hasher2;
    const auto after = detectDuplicates(candidates, reader, hasher2, options);
    SPACELENS_REQUIRE(after.completed);
    SPACELENS_REQUIRE(after.summary.cacheMisses + after.summary.filesFullyHashed >=
                      1);
    if (!after.groups.empty()) {
        SPACELENS_REQUIRE(after.groups.front().contentSha256Hex != beforeHex);
    }
}

SPACELENS_TEST(HashCache_setfiletime_restore_still_misses)
{
    const TempDir dir("setfiletime");
    const ByteSize size = 64ULL * 1024ULL;
    const auto a = dir.path / "a.bin";
    const auto b = dir.path / "b.bin";
    writeBytes(a, patternBytes(static_cast<std::size_t>(size), 0x44));
    writeBytes(b, patternBytes(static_cast<std::size_t>(size), 0x44));

    HANDLE handle = ::CreateFileW(a.wstring().c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SPACELENS_REQUIRE(handle != INVALID_HANDLE_VALUE);
    FILETIME created{}, accessed{}, written{};
    const BOOL gotTimes =
        ::GetFileTime(handle, &created, &accessed, &written);
    ::CloseHandle(handle);
    SPACELENS_REQUIRE(gotTimes);

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    DuplicateScanOptions options;
    options.hashCachePath = dir.dbPath();
    options.minimumSize = 1;
    const auto candidates = twoFileCandidates(a, b, size);
    const auto cold = detectDuplicates(candidates, reader, hasher, options);
    SPACELENS_REQUIRE(cold.completed);
    const auto beforeHex = cold.groups.front().contentSha256Hex;

    writeBytes(a, patternBytes(static_cast<std::size_t>(size), 0x55));
    handle = ::CreateFileW(a.wstring().c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SPACELENS_REQUIRE(handle != INVALID_HANDLE_VALUE);
    const BOOL restored = ::SetFileTime(handle, &created, &accessed, &written);
    ::CloseHandle(handle);
    SPACELENS_REQUIRE(restored);

    WindowsFileContentHasher hasher2;
    const auto after = detectDuplicates(candidates, reader, hasher2, options);
    SPACELENS_REQUIRE(after.completed);
    // Size+mtime+FileId would false-hit here. ChangeTime/USN must force rehash.
    SPACELENS_REQUIRE(after.summary.filesFullyHashed >= 1);
    if (!after.groups.empty()) {
        SPACELENS_REQUIRE(after.groups.front().contentSha256Hex != beforeHex);
    }
}

SPACELENS_TEST(HashCache_rename_never_false_hits)
{
    const TempDir dir("rename");
    const ByteSize size = 48ULL * 1024ULL;
    auto a = dir.path / "named.bin";
    const auto b = dir.path / "copy.bin";
    writeBytes(a, patternBytes(static_cast<std::size_t>(size), 0x66));
    writeBytes(b, patternBytes(static_cast<std::size_t>(size), 0x66));

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    DuplicateScanOptions options;
    options.hashCachePath = dir.dbPath();
    options.minimumSize = 1;
    const auto cold =
        detectDuplicates(twoFileCandidates(a, b, size), reader, hasher, options);
    SPACELENS_REQUIRE(cold.completed);
    const auto hex = cold.groups.front().contentSha256Hex;

    const auto renamed = dir.path / "renamed.bin";
    fs::rename(a, renamed);
    WindowsFileContentHasher hasher2;
    const auto after = detectDuplicates(twoFileCandidates(renamed, b, size),
                                        reader, hasher2, options);
    SPACELENS_REQUIRE(after.completed);
    SPACELENS_REQUIRE_EQ(after.groups.size(), 1ULL);
    SPACELENS_REQUIRE_EQ(after.groups.front().contentSha256Hex, hex);
}

SPACELENS_TEST(HashCache_delete_recreate_same_path_misses)
{
    const TempDir dir("recreate");
    const ByteSize size = 40ULL * 1024ULL;
    const auto a = dir.path / "rec.bin";
    const auto b = dir.path / "copy.bin";
    writeBytes(a, patternBytes(static_cast<std::size_t>(size), 0x77));
    writeBytes(b, patternBytes(static_cast<std::size_t>(size), 0x77));

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    DuplicateScanOptions options;
    options.hashCachePath = dir.dbPath();
    options.minimumSize = 1;
    const auto cold =
        detectDuplicates(twoFileCandidates(a, b, size), reader, hasher, options);
    SPACELENS_REQUIRE(cold.completed);

    fs::remove(a);
    writeBytes(a, patternBytes(static_cast<std::size_t>(size), 0x88));
    WindowsFileContentHasher hasher2;
    const auto after =
        detectDuplicates(twoFileCandidates(a, b, size), reader, hasher2, options);
    SPACELENS_REQUIRE(after.completed);
    SPACELENS_REQUIRE(after.summary.filesFullyHashed >= 1);
}

SPACELENS_TEST(HashCache_hardlink_shares_identity)
{
    const TempDir dir("hardlink");
    const ByteSize size = 32ULL * 1024ULL;
    const auto original = dir.path / "orig.bin";
    const auto alias = dir.path / "alias.bin";
    const auto copy = dir.path / "copy.bin";
    writeBytes(original, patternBytes(static_cast<std::size_t>(size), 0x99));
    writeBytes(copy, patternBytes(static_cast<std::size_t>(size), 0x99));
    if (!::CreateHardLinkW(alias.wstring().c_str(), original.wstring().c_str(),
                           nullptr)) {
        std::cout << "[ SKIP ] HashCache_hardlink_shares_identity — "
                  << "CreateHardLinkW failed, err=" << ::GetLastError() << "\n";
        return;
    }

    WindowsCleanupMetadataReader reader;
    WindowsFileContentHasher hasher;
    DuplicateScanOptions options;
    options.hashCachePath = dir.dbPath();
    options.minimumSize = 1;
    DuplicateCandidateQueryResult candidates;
    candidates.ok = true;
    DuplicateSizeBucket bucket;
    bucket.logicalSize = size;
    bucket.files.push_back(cand(original.wstring(), size));
    bucket.files.push_back(cand(alias.wstring(), size));
    bucket.files.push_back(cand(copy.wstring(), size));
    candidates.buckets.push_back(bucket);
    candidates.candidateFiles = 3;

    const auto first = detectDuplicates(candidates, reader, hasher, options);
    SPACELENS_REQUIRE(first.completed);
    SPACELENS_REQUIRE_EQ(first.groups.front().distinctIdentityCount, 2ULL);

    WindowsFileContentHasher hasher2;
    const auto second = detectDuplicates(candidates, reader, hasher2, options);
    SPACELENS_REQUIRE(second.completed);
    SPACELENS_REQUIRE_EQ(second.groups.front().contentSha256Hex,
                         first.groups.front().contentSha256Hex);
}

SPACELENS_TEST(HashCache_json_telemetry_additive)
{
    DuplicateDetectionResult result;
    result.summary.cacheHits = 2;
    result.summary.cacheMisses = 1;
    result.summary.cacheWrites = 3;
    result.summary.filesFullyHashed = 1;
    result.summary.bytesFullyHashed = 10;
    result.summary.bytesReusedFromCache = 20;
    const auto json = result.toJson();
    SPACELENS_REQUIRE(json.find("\"cache_hits\":2") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"cache_misses\":1") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"cache_writes\":3") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"files_fully_hashed\":1") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"planning_only\":true") != std::string::npos);
    SPACELENS_REQUIRE(json.find("\"filesystem_mutation\":false") !=
                      std::string::npos);
}
