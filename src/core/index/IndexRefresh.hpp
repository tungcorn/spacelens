#pragma once

#include "core/index/IndexPaths.hpp"
#include "core/index/IndexStore.hpp"
#include "platform/windows/UsnJournal.hpp"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>

namespace spacelens {

/// Agent-visible incremental refresh readiness (not a boolean).
enum class IncrementalRefreshState {
    Supported,              // checkpoint valid; refresh may proceed
    Unavailable,            // no checkpoint (full rebuild never stored one)
    UnsupportedFilesystem,  // not NTFS
    AccessDenied,
    JournalNotActive,       // journal missing; we never create it
    JournalChanged,         // UsnJournalID mismatch
    HistoryLost,            // USN range no longer covers checkpoint
    VolumeChanged,          // volume serial mismatch
    NeedsFullRebuild,       // explicit fallback required
    Unknown
};

[[nodiscard]] const char* toString(IncrementalRefreshState state) noexcept;

enum class IndexRefreshOutcome {
    Refreshed,
    AlreadyCurrent,
    FullRebuildRequired,
    Cancelled,
    Failed,
    IndexNotFound
};

[[nodiscard]] const char* toString(IndexRefreshOutcome outcome) noexcept;

struct RefreshCheckpoint {
    std::int64_t rootId = 1;
    std::wstring volumeDevicePath;
    std::wstring volumeRootPath;
    std::uint32_t volumeSerial = 0;
    std::wstring filesystem;
    std::uint64_t usnJournalId = 0;
    std::uint64_t nextUsn = 0;
    std::uint64_t lowestValidUsnAtCapture = 0;
    std::uint64_t fullIndexedAtTicks = 0;
    std::uint64_t lastRefreshAtTicks = 0;
    std::string lastRefreshMethod;  // "full" | "usn"
    std::string status;             // "ready" | "unavailable" | "needs_full_rebuild"
};

struct IndexRefreshResult {
    IndexRefreshOutcome outcome = IndexRefreshOutcome::Failed;
    IncrementalRefreshState incrementalState = IncrementalRefreshState::Unknown;
    std::string reason;  // machine-readable reason code
    IndexLocation location{};
    IndexRootInfo root{};
    RefreshCheckpoint checkpoint{};

    std::uint64_t journalRecordsSeen = 0;
    std::uint64_t recordsInRoot = 0;
    std::uint64_t added = 0;
    std::uint64_t modified = 0;
    std::uint64_t removed = 0;
    std::uint64_t renamed = 0;
    std::uint64_t dirsRecomputed = 0;
    std::uint64_t rowsChanged = 0;
    double elapsedSeconds = 0.0;
    std::string error;

    /// Diagnostics (always filled on refresh attempts that open the journal).
    /// Safe for agents/tests; not a substitute for outcome/reason.
    std::uint64_t diagStartUsn = 0;          // checkpoint next_usn requested
    std::uint64_t diagJournalNextUsn = 0;    // live journal NextUsn at read start
    std::uint64_t diagJournalLowestUsn = 0;  // live LowestValidUsn
    std::uint64_t diagContinuationUsn = 0;   // driver continuation after read
    std::uint64_t diagCommittedNextUsn = 0;  // checkpoint next_usn after commit (0 if not)
    std::uint64_t diagCoalescedFrns = 0;
};

/// Probe whether incremental refresh is possible for a published index.
[[nodiscard]] IndexRefreshResult probeIncremental(const std::wstring& rootPath);

/// Apply USN-based incremental refresh to a published index.
/// Atomic: checkpoint advances only if the delta transaction commits.
/// On FullRebuildRequired, the previous index remains valid and queryable.
[[nodiscard]] IndexRefreshResult refreshIndex(const std::wstring& rootPath,
                                              std::stop_token stop = {});

/// Capture a USN checkpoint after a successful full rebuild (best-effort).
/// Does not fail the full build if the journal is unavailable.
void writeRefreshCheckpointAfterFullBuild(IndexStore& store,
                                          const std::wstring& rootPath,
                                          std::uint64_t fullIndexedAtTicks);

/// Read the persisted refresh checkpoint, if any. Missing/unreadable → nullopt.
[[nodiscard]] std::optional<RefreshCheckpoint> readRefreshCheckpoint(SqliteDb& db);

}  // namespace spacelens
