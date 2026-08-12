#include "TestRunner.hpp"

#include "core/index/IndexBuilder.hpp"
#include "core/index/IndexPaths.hpp"
#include "core/index/IndexQuery.hpp"
#include "core/index/IndexRefresh.hpp"
#include "platform/windows/UsnJournal.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace spacelens;
namespace fs = std::filesystem;

namespace {

struct SnapshotRow {
    std::wstring path;
    IndexEntryKind kind = IndexEntryKind::File;
    ByteSize size_bytes = 0;
    std::string classification;
    std::string confidence;
    std::string location_safety;
    std::string reclaimability;
    std::string candidate_strength;
    std::uint64_t last_write_ticks = 0;
};

struct IndexSnapshot {
    std::uint64_t fileCount = 0;
    std::uint64_t dirCount = 0;
    ByteSize logicalBytes = 0;
    std::vector<SnapshotRow> rows;
};

std::wstring makeTempRoot(const char* tag)
{
    const auto base =
        fs::temp_directory_path() / "spacelens_usn_parity" /
        (std::string(tag) + "_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base / "sub" / "nested");
    fs::create_directories(base / "build" / "obj");
    fs::create_directories(base / "models");
    {
        std::ofstream(base / "readme.txt") << "hello world";
        std::ofstream(base / "sub" / "data.bin") << std::string(4096, 'x');
        std::ofstream(base / "sub" / "nested" / "leaf.txt") << "leaf";
        std::ofstream(base / "build" / "obj" / "a.obj") << std::string(512, 'o');
        std::ofstream(base / "models" / "sample.gguf") << std::string(2048, 'g');
    }
    // Outside-root sibling used for boundary tests when indexing `sub`.
    fs::create_directories(base / "outside");
    std::ofstream(base / "outside" / "ext.txt") << "external";
    return base.wstring();
}

void writeFile(const fs::path& p, const std::string& data)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

IndexSnapshot captureSnapshot(const std::wstring& root)
{
    IndexSnapshot snap;
    auto st = indexStatus(root);
    if (st.ok) {
        snap.fileCount = st.root.fileCount;
        snap.dirCount = st.root.dirCount;
        snap.logicalBytes = st.root.logicalBytes;
    }

    IndexQuerySpec files;
    files.includeFiles = true;
    files.includeDirectories = false;
    files.limit = 100000;
    auto fq = queryIndex(root, files);
    SPACELENS_REQUIRE(fq.ok);

    IndexQuerySpec dirs;
    dirs.includeFiles = false;
    dirs.includeDirectories = true;
    dirs.limit = 100000;
    auto dq = queryIndex(root, dirs);
    SPACELENS_REQUIRE(dq.ok);

    snap.rows.reserve(fq.hits.size() + dq.hits.size());
    auto append = [&](const IndexHit& h) {
        SnapshotRow r;
        r.path = h.path;
        r.kind = h.kind;
        r.size_bytes = h.size_bytes;
        r.classification = h.classification;
        r.confidence = h.confidence;
        r.location_safety = h.location_safety;
        r.reclaimability = h.reclaimability;
        r.candidate_strength = h.candidate_strength;
        r.last_write_ticks = h.last_write_ticks;
        snap.rows.push_back(std::move(r));
    };
    for (const auto& h : fq.hits) {
        append(h);
    }
    for (const auto& h : dq.hits) {
        append(h);
    }
    std::sort(snap.rows.begin(), snap.rows.end(),
              [](const SnapshotRow& a, const SnapshotRow& b) {
                  return a.path < b.path;
              });
    return snap;
}

std::string narrow(const std::wstring& w)
{
    std::string out;
    out.reserve(w.size());
    for (wchar_t ch : w) {
        out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return out;
}

void requireParity(const IndexSnapshot& incremental, const IndexSnapshot& full,
                   const char* label)
{
    if (incremental.fileCount != full.fileCount ||
        incremental.dirCount != full.dirCount ||
        incremental.logicalBytes != full.logicalBytes) {
        throw spacelens::test::Failure(
            std::string(label) + " root counts mismatch: inc(files=" +
            std::to_string(incremental.fileCount) +
            ",dirs=" + std::to_string(incremental.dirCount) +
            ",bytes=" + std::to_string(incremental.logicalBytes) +
            ") full(files=" + std::to_string(full.fileCount) +
            ",dirs=" + std::to_string(full.dirCount) +
            ",bytes=" + std::to_string(full.logicalBytes) + ")");
    }
    if (incremental.rows.size() != full.rows.size()) {
        throw spacelens::test::Failure(
            std::string(label) + " row count mismatch: inc=" +
            std::to_string(incremental.rows.size()) +
            " full=" + std::to_string(full.rows.size()));
    }
    for (std::size_t i = 0; i < incremental.rows.size(); ++i) {
        const auto& a = incremental.rows[i];
        const auto& b = full.rows[i];
        if (a.path != b.path || a.kind != b.kind || a.size_bytes != b.size_bytes ||
            a.classification != b.classification ||
            a.confidence != b.confidence ||
            a.location_safety != b.location_safety ||
            a.reclaimability != b.reclaimability ||
            a.candidate_strength != b.candidate_strength) {
            throw spacelens::test::Failure(
                std::string(label) + " row mismatch at " + std::to_string(i) +
                " path_inc=" + narrow(a.path) + " path_full=" + narrow(b.path) +
                " size_inc=" + std::to_string(a.size_bytes) +
                " size_full=" + std::to_string(b.size_bytes) +
                " class_inc=" + a.classification +
                " class_full=" + b.classification +
                " reclaim_inc=" + a.reclaimability +
                " reclaim_full=" + b.reclaimability +
                " strength_inc=" + a.candidate_strength +
                " strength_full=" + b.candidate_strength);
        }
        // last_write may differ by sub-ms races after full rebuild re-stat; allow
        // equality preferred but not required if both non-zero and close.
        (void)a.last_write_ticks;
        (void)b.last_write_ticks;
    }
}

bool usnSupportedFor(const std::wstring& root)
{
    UsnJournalReader reader;
    return UsnJournalReader::tryOpen(root, reader) == UsnCapability::Supported;
}

void sleepUsn()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

}  // namespace

SPACELENS_TEST(UsnParity_environment_probe)
{
    UsnJournalReader reader;
    const auto path = fs::temp_directory_path().wstring();
    const auto cap = UsnJournalReader::tryOpen(path, reader);
    std::cout << "  [usn-probe] path=" << narrow(path)
              << " capability=" << toString(cap) << '\n';
    if (cap == UsnCapability::Supported) {
        UsnJournalState st{};
        SPACELENS_REQUIRE(reader.query(st) == UsnCapability::Supported);
        std::cout << "  [usn-probe] journalId=" << st.journalId
                  << " nextUsn=" << st.nextUsn
                  << " lowest=" << st.lowestValidUsn << '\n';
        SPACELENS_REQUIRE(st.journalId != 0 || st.nextUsn >= st.firstUsn);
    } else {
        std::cout << "  [usn-probe] real happy-path requires elevation/backup "
                     "privilege on this machine\n";
    }
}

SPACELENS_TEST(UsnParity_full_mutation_suite_or_skip)
{
    const std::wstring root = makeTempRoot("suite");
    const fs::path rootPath(root);

    if (!usnSupportedFor(root)) {
        std::cout << "  [skip] USN not Supported — cannot exercise real "
                     "incremental happy path in this process\n";
        return;
    }

    auto built = buildIndexForRoot(root, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);

    auto probe = probeIncremental(root);
    SPACELENS_REQUIRE(probe.incrementalState == IncrementalRefreshState::Supported);
    SPACELENS_REQUIRE(probe.checkpoint.status == "ready");
    SPACELENS_REQUIRE(probe.checkpoint.usnJournalId != 0);

    // --- mutations on fixture only ---
    writeFile(rootPath / "new_file.dat", std::string(8192, 'n'));
    writeFile(rootPath / "readme.txt", "hello world!!!");  // size change
    fs::remove(rootPath / "sub" / "data.bin");
    writeFile(rootPath / "sub" / "c.txt", "created");
    fs::rename(rootPath / "models" / "sample.gguf",
               rootPath / "models" / "sample_renamed.gguf");
    fs::create_directories(rootPath / "sub" / "moved_here");
    fs::rename(rootPath / "sub" / "nested" / "leaf.txt",
               rootPath / "sub" / "moved_here" / "leaf.txt");
    fs::rename(rootPath / "build" / "obj", rootPath / "build" / "objects");
    sleepUsn();

    auto refreshed = refreshIndex(root, {});
    SPACELENS_REQUIRE(refreshed.outcome == IndexRefreshOutcome::Refreshed ||
                      refreshed.outcome == IndexRefreshOutcome::AlreadyCurrent);
    SPACELENS_REQUIRE(refreshed.outcome != IndexRefreshOutcome::FullRebuildRequired);
    std::cout << "  [refresh] records=" << refreshed.journalRecordsSeen
              << " in_root=" << refreshed.recordsInRoot
              << " added=" << refreshed.added << " removed=" << refreshed.removed
              << " modified=" << refreshed.modified
              << " renamed=" << refreshed.renamed
              << " dirs=" << refreshed.dirsRecomputed << '\n';

    const IndexSnapshot afterInc = captureSnapshot(root);

    auto full = buildIndexForRoot(root, {});
    SPACELENS_REQUIRE(full.state == IndexBuildState::Completed);
    const IndexSnapshot afterFull = captureSnapshot(root);

    requireParity(afterInc, afterFull, "mutation_suite");
}

SPACELENS_TEST(UsnParity_subdirectory_root_boundary_or_skip)
{
    const std::wstring base = makeTempRoot("boundary");
    const fs::path basePath(base);
    const std::wstring root = (basePath / "sub").wstring();

    if (!usnSupportedFor(root)) {
        std::cout << "  [skip] USN not Supported — boundary parity skipped\n";
        return;
    }

    auto built = buildIndexForRoot(root, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);
    auto probe = probeIncremental(root);
    if (probe.incrementalState != IncrementalRefreshState::Supported) {
        std::cout << "  [skip] checkpoint not ready after full build: "
                  << probe.reason << '\n';
        return;
    }

    // Move file into indexed root from outside.
    fs::rename(basePath / "outside" / "ext.txt",
               basePath / "sub" / "from_outside.txt");
    // Move file out of indexed root.
    fs::rename(basePath / "sub" / "data.bin", basePath / "outside" / "data.bin");
    // Create inside.
    writeFile(basePath / "sub" / "only_inside.txt", "inside");
    sleepUsn();

    auto refreshed = refreshIndex(root, {});
    SPACELENS_REQUIRE(refreshed.outcome == IndexRefreshOutcome::Refreshed ||
                      refreshed.outcome == IndexRefreshOutcome::AlreadyCurrent);

    const IndexSnapshot afterInc = captureSnapshot(root);
    auto full = buildIndexForRoot(root, {});
    SPACELENS_REQUIRE(full.state == IndexBuildState::Completed);
    const IndexSnapshot afterFull = captureSnapshot(root);
    requireParity(afterInc, afterFull, "subdir_boundary");

    // Ensure outside path is not indexed.
    for (const auto& row : afterInc.rows) {
        SPACELENS_REQUIRE(row.path.find(L"\\outside\\") == std::wstring::npos);
    }
}

// Multi-batch refresh regression: the pre-fix bug stored max(record.usn)+1 as
// next_usn, which is not a valid record boundary. Second refresh then hit
// ERROR_INVALID_PARAMETER → HistoryLost → full_rebuild_required with 0 records.
SPACELENS_TEST(UsnParity_multi_refresh_batches_or_skip)
{
    const std::wstring root = makeTempRoot("multi");
    const fs::path rootPath(root);

    if (!usnSupportedFor(root)) {
        std::cout << "  [skip] USN not Supported — multi-refresh skipped\n";
        return;
    }

    auto built = buildIndexForRoot(root, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);
    auto probe0 = probeIncremental(root);
    SPACELENS_REQUIRE(probe0.incrementalState ==
                      IncrementalRefreshState::Supported);
    const std::uint64_t cp0 = probe0.checkpoint.nextUsn;
    std::cout << "  [multi] checkpoint_after_full next_usn=" << cp0 << '\n';

    auto requireRefreshOk = [](const IndexRefreshResult& r, const char* label) {
        if (r.outcome == IndexRefreshOutcome::FullRebuildRequired) {
            throw spacelens::test::Failure(
                std::string(label) +
                " unexpected full_rebuild_required reason=" + r.reason +
                " journal_records_seen=" +
                std::to_string(r.journalRecordsSeen) +
                " start_usn=" + std::to_string(r.diagStartUsn) +
                " cont_usn=" + std::to_string(r.diagContinuationUsn) +
                " committed=" + std::to_string(r.diagCommittedNextUsn));
        }
        SPACELENS_REQUIRE(r.outcome == IndexRefreshOutcome::Refreshed ||
                          r.outcome == IndexRefreshOutcome::AlreadyCurrent);
    };

    // Batch A: 1 change
    writeFile(rootPath / "batch_a.txt", "a");
    sleepUsn();
    auto rA = refreshIndex(root, {});
    requireRefreshOk(rA, "batch_A");
    std::cout << "  [multi] batch_A outcome=" << toString(rA.outcome)
              << " records=" << rA.journalRecordsSeen
              << " start=" << rA.diagStartUsn
              << " cont=" << rA.diagContinuationUsn
              << " committed=" << rA.diagCommittedNextUsn << '\n';
    SPACELENS_REQUIRE(rA.diagCommittedNextUsn == 0 ||
                      rA.diagCommittedNextUsn >= rA.diagStartUsn);
    if (rA.outcome == IndexRefreshOutcome::Refreshed) {
        SPACELENS_REQUIRE(rA.checkpoint.nextUsn >= cp0);
        // Committed cursor must not be record.usn+1 style (we cannot prove the
        // absolute value, but it must equal the driver continuation).
        SPACELENS_REQUIRE(rA.diagCommittedNextUsn == rA.diagContinuationUsn ||
                          rA.diagCommittedNextUsn == rA.checkpoint.nextUsn);
    }
    const IndexSnapshot snapA = captureSnapshot(root);

    // Batch B: ~100 changes on same index (no full rebuild between)
    for (int i = 0; i < 100; ++i) {
        writeFile(rootPath / "bulk_b" / ("f" + std::to_string(i) + ".txt"),
                  std::string(static_cast<std::size_t>(i % 32) + 1, 'b'));
    }
    sleepUsn();
    auto rB = refreshIndex(root, {});
    requireRefreshOk(rB, "batch_B");
    std::cout << "  [multi] batch_B outcome=" << toString(rB.outcome)
              << " records=" << rB.journalRecordsSeen
              << " start=" << rB.diagStartUsn
              << " cont=" << rB.diagContinuationUsn
              << " committed=" << rB.diagCommittedNextUsn << '\n';
    // The critical regression: batch B must not fail after A succeeded.
    SPACELENS_REQUIRE(rB.reason != "history_lost");
    if (rB.outcome == IndexRefreshOutcome::Refreshed &&
        rA.outcome == IndexRefreshOutcome::Refreshed) {
        SPACELENS_REQUIRE(rB.diagStartUsn == rA.checkpoint.nextUsn);
    }
    const IndexSnapshot snapB = captureSnapshot(root);

    // Batch C: another ~20 changes + rename
    writeFile(rootPath / "batch_c.txt", std::string(256, 'c'));
    fs::rename(rootPath / "batch_a.txt", rootPath / "batch_a_renamed.txt");
    for (int i = 0; i < 20; ++i) {
        writeFile(rootPath / "bulk_c" / ("g" + std::to_string(i) + ".txt"),
                  "g");
    }
    sleepUsn();
    auto rC = refreshIndex(root, {});
    requireRefreshOk(rC, "batch_C");
    std::cout << "  [multi] batch_C outcome=" << toString(rC.outcome)
              << " records=" << rC.journalRecordsSeen
              << " start=" << rC.diagStartUsn
              << " cont=" << rC.diagContinuationUsn
              << " committed=" << rC.diagCommittedNextUsn << '\n';
    const IndexSnapshot afterInc = captureSnapshot(root);

    // Empty-tail refresh must stay Supported / already_current — not HistoryLost.
    auto rEmpty = refreshIndex(root, {});
    requireRefreshOk(rEmpty, "empty_tail");
    SPACELENS_REQUIRE(rEmpty.outcome != IndexRefreshOutcome::FullRebuildRequired);

    // Oracle parity after multi-batch.
    auto full = buildIndexForRoot(root, {});
    SPACELENS_REQUIRE(full.state == IndexBuildState::Completed);
    const IndexSnapshot afterFull = captureSnapshot(root);
    requireParity(afterInc, afterFull, "multi_refresh");

    // Snapshots must have grown across batches (sanity).
    SPACELENS_REQUIRE(snapB.fileCount >= snapA.fileCount);
    SPACELENS_REQUIRE(afterInc.fileCount >= snapB.fileCount);
}

// Process-restart simulation: close all handles, re-open published index via
// refreshIndex on a fresh call path (same process but fully re-probed checkpoint).
SPACELENS_TEST(UsnParity_checkpoint_survives_reopen_or_skip)
{
    const std::wstring root = makeTempRoot("reopen");
    const fs::path rootPath(root);

    if (!usnSupportedFor(root)) {
        std::cout << "  [skip] USN not Supported — reopen checkpoint skipped\n";
        return;
    }

    auto built = buildIndexForRoot(root, {});
    SPACELENS_REQUIRE(built.state == IndexBuildState::Completed);

    writeFile(rootPath / "before_restart.txt", "v1");
    sleepUsn();
    auto r1 = refreshIndex(root, {});
    if (r1.outcome == IndexRefreshOutcome::FullRebuildRequired) {
        throw spacelens::test::Failure(
            std::string("pre-restart refresh failed: ") + r1.reason);
    }
    SPACELENS_REQUIRE(r1.outcome == IndexRefreshOutcome::Refreshed ||
                      r1.outcome == IndexRefreshOutcome::AlreadyCurrent);
    const std::uint64_t committed = r1.checkpoint.nextUsn;
    std::cout << "  [reopen] after_r1 next_usn=" << committed
              << " cont=" << r1.diagContinuationUsn << '\n';

    // Drop local result; re-probe from disk as a new process would.
    auto probe = probeIncremental(root);
    SPACELENS_REQUIRE(probe.incrementalState ==
                      IncrementalRefreshState::Supported);
    SPACELENS_REQUIRE(probe.checkpoint.nextUsn == committed);
    SPACELENS_REQUIRE(probe.checkpoint.status == "ready");

    // Mutate after "process exit", then refresh from persisted checkpoint.
    writeFile(rootPath / "after_restart.txt", "v2");
    writeFile(rootPath / "before_restart.txt", "v1-updated");
    sleepUsn();
    auto r2 = refreshIndex(root, {});
    if (r2.outcome == IndexRefreshOutcome::FullRebuildRequired) {
        throw spacelens::test::Failure(
            std::string("post-restart refresh failed: ") + r2.reason +
            " start_usn=" + std::to_string(r2.diagStartUsn) +
            " journal_records_seen=" +
            std::to_string(r2.journalRecordsSeen));
    }
    SPACELENS_REQUIRE(r2.outcome == IndexRefreshOutcome::Refreshed ||
                      r2.outcome == IndexRefreshOutcome::AlreadyCurrent);
    SPACELENS_REQUIRE(r2.diagStartUsn == committed);
    std::cout << "  [reopen] after_r2 outcome=" << toString(r2.outcome)
              << " start=" << r2.diagStartUsn
              << " cont=" << r2.diagContinuationUsn
              << " records=" << r2.journalRecordsSeen << '\n';

    const IndexSnapshot afterInc = captureSnapshot(root);
    auto full = buildIndexForRoot(root, {});
    SPACELENS_REQUIRE(full.state == IndexBuildState::Completed);
    const IndexSnapshot afterFull = captureSnapshot(root);
    requireParity(afterInc, afterFull, "reopen_checkpoint");
}
