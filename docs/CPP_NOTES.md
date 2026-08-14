# C++ Notes

Starter notes for implementation. Extend this file when a recurring C++ or Windows-specific decision needs to be recorded.

## RAII and `std::unique_ptr`

- **Why used:** Resource ownership should be visible in the type and released automatically on every return path, including errors and cancellation.
- **Ownership:** The object that acquires a resource owns it. Use value types for ordinary records and `std::unique_ptr` for exclusive ownership of polymorphic or separately allocated objects.
- **Lifetime:** The resource lives exactly as long as its wrapper or owning object; destruction is deterministic and scope-based.
- **Bug prevented:** Leaks, double deletion, and cleanup being skipped by an exception or early return. Avoid `shared_ptr` for the scan tree unless shared ownership is an explicit requirement.

## Windows search-handle wrappers

- **Why used:** `FindFirstFileExW` returns a search handle that must be closed with `FindClose`, not the generic `CloseHandle` API.
- **Ownership:** A small move-only RAII wrapper exclusively owns one valid `HANDLE`/search handle and closes it in its destructor. Copying is disabled; moving transfers ownership.
- **Lifetime:** The wrapper remains alive for the complete `FindNextFileW` loop and closes the handle when the loop ends or an error interrupts it.
- **Bug prevented:** Handle leaks, invalid close functions, accidental handle duplication, and cleanup failures on exceptions or cancellation.

## `std::jthread` and `std::stop_token`

- **Why used:** Asynchronous scanning needs bounded worker lifetime and cooperative cancellation. `std::jthread` joins automatically and supplies a stop token to the worker.
- **Ownership:** `ScanSession` or the app-level scan coordinator owns the worker objects. The worker does not own the session or UI.
- **Lifetime:** A worker lasts for one scan operation. Request stop before destroying the session; destruction then joins before session-owned state is released.
- **Bug prevented:** Detached threads accessing destroyed result/UI state, forgotten joins, and cancellation flags with data races. Workers still check the token at defined traversal/batch boundaries because filesystem calls are not forcibly interrupted.

## `std::string_view` usage boundaries

- **Why used:** Non-owning views avoid needless copies while parsing or comparing names that already have a guaranteed owner.
- **Ownership:** The caller owns the underlying string. A view may not escape the scope or operation that keeps that string alive.
- **Lifetime:** Convert to an owning `std::string`/path representation when storing a name in `DirectoryTree`, crossing a thread boundary, returning from a function whose input may die, or handing data to Qt/UI code.
- **Bug prevented:** Dangling references caused by views into temporary strings, reallocated buffers, or Win32 callback storage. Do not use a view as a persistent field unless the owner and lifetime are explicit.

## Top-K and `std::priority_queue`

- **Why used:** The largest-file view needs only the best `K` records, not a sorted copy of all `N` files.
- **Ownership:** The scan owns the heap entries for the current session. Entries should contain the data needed after enumeration, including an owning path/name representation where required.
- **Lifetime:** The heap lives for one scan and is converted into an ordered result when the scan completes or publishes a snapshot.
- **Bug prevented:** Unbounded memory growth and unnecessary `O(N log N)` full sorting. A min-heap keeps the smallest retained candidate at the top, so each replacement costs `O(log K)` and total maintenance is `O(N log K)`.

## Index-based `DirectoryTree`

- **Why used:** A disk tree may contain many directories and files. Indices into owned contiguous/stable storage make ownership and teardown simple without a `shared_ptr` graph.
- **Ownership:** `DirectoryTree` owns all node and file records. Parent/child relationships are integer indices; indices are invalid after the owning storage is destroyed or reallocated according to the container contract.
- **Lifetime:** The tree and every index referring to it belong to one `ScanSession` result. UI selections must be translated into stable IDs or refreshed snapshots rather than retaining raw references across mutations.
- **Bug prevented:** Reference cycles, accidental shared ownership, recursive destruction overhead, and unclear mutation/lifetime rules. It also supports reconstructing paths from parent indices instead of duplicating full paths for every node.

## `IFileEnumerator` abstraction

- **Why used:** Keeps `ScanEngine` free of Win32 so tests can inject a fake filesystem and a future NTFS fast path can plug in without rewriting aggregation.
- **Ownership:** The caller owns the enumerator instance; `ScanEngine` only borrows a reference for the duration of `scan()`.
- **Lifetime:** The enumerator must outlive the scan call. Fake enumerators live on the test stack; the production Windows enumerator is owned by the app/session.
- **Bug prevented:** Core logic coupled to live disks (flaky tests) and duplicated scan algorithms per platform backend.

## Reparse-point policy

- **Why used:** Junctions and directory symlinks can create cycles or double-count storage if followed blindly.
- **Ownership:** Classification is done by the platform enumerator (`EntryKind::ReparseDirectory`); the engine decides whether to recurse via `ScanOptions`.
- **Lifetime:** Policy is fixed for a single scan operation.
- **Bug prevented:** Infinite recursion through junction loops and inflated totals when the same files appear under multiple linked paths.

## Windows case-insensitive executables

- **Why it matters:** NTFS treats `spacelens.exe` and `SpaceLens.exe` as the same path. Linking both CLI and GUI into `build/` under those names overwrites one binary with the other (and can launch the GUI when you meant the CLI).
- **Ownership:** CMake assigns distinct `OUTPUT_NAME` / `RUNTIME_OUTPUT_DIRECTORY` values (`build/cli/spacelens.exe` vs `build/gui/spacelens-gui.exe`).
- **Lifetime:** Applies for the whole Windows product layout.
- **Bug prevented:** “CLI opens a window / produces no stdout” caused by accidentally executing the Qt GUI binary.

## `ScanSession` + `std::jthread` with Qt signals

- **Why used:** Keep the GUI responsive while `ScanEngine` walks the filesystem. `std::jthread` joins on destruction and exposes `std::stop_token` for cooperative cancel.
- **Ownership:** `MainWindow` owns `ScanSession` via `unique_ptr`. The session owns the worker thread and the latest `ScanResult` until the UI calls `takeResult()`.
- **Lifetime:** Starting a scan spawns one worker. Cancel or destruction calls `request_stop()` then `join()`. Progress/completion are delivered with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` so slots run on the GUI thread after the worker posts them.
- **Bug prevented:** Scanning on the GUI thread (frozen UI), use-after-free if the window closes mid-scan, and cross-thread Qt widget access. Progress is throttled inside `ScanEngine` so the event queue is not flooded.

## Volume handle + USN journal (read-only)

- **Why used:** Incremental Index V2 needs volume identity and NTFS USN records without mutating the journal or the analyzed tree.
- **Ownership:** `VolumeHandle` is move-only RAII over a Win32 `HANDLE` closed with `CloseHandle`. `UsnJournalReader` owns one volume handle for the duration of probe/refresh.
- **Lifetime:** Open for a single probe or refresh operation; never cache a volume handle across process-lifetime background watchers in V2.
- **Bug prevented:** Accidental write access to volumes, journal create/delete FSCTLs, and treating weak open failures as “non-NTFS”. On NTFS, USN IOCTL failures map to `access_denied` when rights are insufficient.
- **Privilege note:** Opening `\\.\X:` usually requires admin or `SeBackupPrivilege`. The open path best-effort enables backup/restore privileges already present on the token; it does not elevate.
- **READ continuation cursor:** `FSCTL_READ_USN_JOURNAL` returns the next `StartUsn`
  in the first 8 bytes of the output buffer. Persist that value (or journal
  `NextUsn` when caught up) as `refresh_checkpoint.next_usn`. Do **not** store
  `record.usn + 1` — USNs are sparse record-boundary offsets; a non-boundary
  `StartUsn` yields `ERROR_INVALID_PARAMETER` mapped to `HistoryLost`.
- **Empty matching buffer:** continue the read loop while the driver continuation
  advances and remains below the journal tip. Returning early on `!any` left the
  cursor mid-range and combined with `usn+1` math broke the next refresh.

## File identity (FRN) and OpenFileById

- **Why used:** USN records identify objects by file reference number, not path. Refresh resolves paths via `OpenFileById` + `GetFinalPathNameByHandle` and filters with `pathIsUnderRoot`.
- **Ownership:** Identity values are plain structs; no long-lived file handles from identity queries.
- **Bug prevented:** Applying volume-wide USN noise outside the indexed subdirectory root; orphan inserts when a parent directory is missing under the root (force full rebuild instead).
- **8.3 vs long path:** `GetFinalPathNameByHandle(..., FILE_NAME_NORMALIZED)` returns
  long names (`C:\Users\runneradmin\...`). `GetTempPath` / `std::filesystem::temp_directory_path`
  on some Windows Server images returns 8.3 (`C:\Users\RUNNER~1\...`).
  `pathIsUnderRoot` must expand both sides with `GetLongPathNameW` before the
  prefix compare; otherwise incremental refresh treats in-root creates as outside
  and existing rows as moved-out (deleted from the index). After a path is
  accepted as in-root, `rebasePathOntoRoot` rewrites it to the indexed root's
  lexical form so stored paths match a full walk and `findEntryIdByPath` /
  `ensureDirUnderRoot` can see the existing 8.3 root row. Do not change
  `normalizeIndexRoot` (that would orphan existing 8.3-keyed indexes).

## Indexed discovery queries (Index Browser V2)

- **Why used:** The GUI must answer “what is large / old / developer / reclaimable”
  from the published SQLite index without ad-hoc SQL in widgets and without a live rescan.
- **Ownership:** `IndexQuerySpec` / `IndexDiscoveryPreset` live in core
  (`IndexQuery`, `IndexCatalog`). `IndexBrowserPage` owns only UI state and an
  `IndexHitTableModel`; query work runs on a page-local `std::jthread` with a
  generation counter so late results are dropped after cancel or a newer query.
- **Lifetime:** One short-lived read-only `IndexStore` connection per
  `queryIndex` call. `browsePath` restricts to immediate children via `parent_id`.
- **Search:** `LIKE %needle% ESCAPE '\'` with `COLLATE NOCASE` on name/path/extension;
  user `%` / `_` / `\` are escaped so matching is literal substring.
- **Bug prevented:** SQL in Qt, unbounded full-table UI loads (limit + matched
  count), treating snapshot paths as live truth without existence checks, and
  blocking the GUI thread on large COUNT/SUM queries.

## Squarified treemap layout (core)

- **Why used:** Visual “where did storage go?” for the current indexed location
  without embedding charting frameworks or SQL in `paintEvent`.
- **Ownership:** `TreemapLayout` is pure geometry over weight items. `IndexOverview`
  builds hierarchy children + overview metrics. `TreemapWidget` owns only display
  state and painted rectangles; it never opens SQLite or enumerates the filesystem.
- **Lifetime:** Layout is recomputed when items or widget size change; paint iterates
  cached rectangles. Hierarchy + discovery queries share the browser page’s
  generation-cancelled `jthread`.
- **Semantics:** Weights are logical bytes. Immediate children only; drill-down
  navigates. “Other” is a visualization aggregate (not a path / review candidate).
  Overview intelligence metrics are **counts**, never overlapping recursive size sums.
- **Bug prevented:** Misleading “reclaimable GB” totals, non-deterministic layout
  from unordered maps, UI freezes from layout-in-paint or DB-in-paint, and
  treating Other as a deletable item.

## Cleanup candidate ownership

- **Why used:** Cleanup Review is a planning queue, not a filesystem-operation owner.
- **Ownership:** `CleanupReview` owns a `std::vector<CleanupCandidate>` by value. A candidate owns its path, captured/current evidence, identity, and validation; it does not own a file handle, directory, or permission to mutate the filesystem. `CleanupReviewController` owns the durable store and is the only mutation path.
- **Lifetime:** Candidates persist in `%LOCALAPPDATA%\SpaceLens\state.db` across process restarts. In-memory state is swapped only after a successful transaction. Snapshot metadata is captured evidence, not current filesystem truth, until the user revalidates or refreshes.
- **Bug prevented:** Review state accidentally becoming delete authority, dangling references to scan or index entries, and losing planning evidence when an index is rebuilt.

## Durable review store

- **Why used:** Review evidence must survive closing SpaceLens and must not be tied to a replaceable `indexes/*/index.db`.
- **Ownership:** `CleanupReviewStore` owns one `SqliteDb` at `spaceLensReviewStatePath()`. Schema marker is independent: `review_schema_version = 1`. Tests inject a temporary database path.
- **Lifetime:** `openDefault()` loads items and last validation without probing the filesystem. Mutations draft a `CleanupReview`, persist the whole operation, then swap memory after commit. Failed writes leave both memory and disk unchanged.
- **Bug prevented:** Index rebuild/refresh silently rewriting review rows; partial validation batches surviving cancel; newer/malformed schemas being “fixed” by replacing a valid database.

## Object identity for review

- **Why used:** Path equality cannot tell a deleted-and-recreated file from the same object.
- **Ownership:** `CleanupIdentity` stores source plus either `VolumeSerialNumber + FILE_ID_128` (`GetFileInformationByHandleEx` / `FileIdInfo`) or the weaker `BY_HANDLE_FILE_INFORMATION` 64-bit file-index fallback. Source is part of equality.
- **Lifetime:** Identity is captured best-effort at add time and compared only during explicit revalidation. Unavailable identity is represented explicitly; zero is never a match. The two forms never compare as equivalent.
- **Bug prevented:** Treating a same-path replacement as unchanged, merging `FILE_ID_128` with a 64-bit fallback, and following a reparse point just to make comparison succeed.

## Cleanup revalidation session

- **Why used:** Metadata probes must not freeze the GUI or commit a half-finished pass.
- **Ownership:** `MainWindow` owns `CleanupRevalidationSession`. The session owns one `std::jthread` and snapshots candidates in stable order. The modal dialog observes signals; it does not own the worker.
- **Lifetime:** One probe at a time, `stop_token` between candidates, queued progress/completion on the GUI thread. Cancellation clears partial updates. Destruction requests stop and joins. Only a completed batch is applied through `replaceValidationBatch`.
- **Bug prevented:** Callbacks into a destroyed dialog, committing cancelled probes, one thread per candidate, and treating a failed persist as an in-memory success.

## User-declared ordinary locations

- **Why used:** Built-in `classifyLocation` must stay path-only. Many legitimate user-managed trees on data volumes are `Unknown` and must not become Ordinary by drive letter.
- **Ownership:** `OrdinaryLocationPolicy` is a value snapshot. `CleanupReviewStore` owns the additive `ordinary_location_declarations` table in the same `state.db` as review/maintenance. `WindowsVolumeIdentityReader` supplies serial plus optional GUID.
- **Lifetime:** Add/remove increment `location_declaration_generation`. Refresh updates status only. Matching uses component-aware ancestry after stripping `\\?\` prefixes. Active declarations never override Protected or Sensitive. Serial `0` persists as `VolumeUnavailable` and cannot authorize maintenance.
- **Bug prevented:** `D:\proj` matching `D:\project`; treating a remounted volume with the same letter as the declared volume; following a reparse root to validate a declaration; CLI/AI writing ordinary-root grants.

## Maintenance Recycle Bin adapter (GUI only)

- **Why used:** Recycle-Bin-only mutation must not leak into the CLI or into core.
- **Ownership:** `spacelens_maintenance` owns `WindowsRecycleAdapter`. `MainWindow` owns `MaintenanceSession`. The session owns one `std::jthread`; the adapter `CoInitializeEx`s STA around each `IFileOperation`. A heap `RecycleSink` keeps its creating reference until after `Unadvise`.
- **Lifetime:** Prepare probes on the worker, wait for a GUI confirmation that says “Move eligible files to Recycle Bin”, then recycle one file at a time. `Attempting` is checkpointed on the GUI thread via `BlockingQueuedConnection` before Shell; Recycled/Uncertain follow. The worker join pumps `ExcludeUserInputEvents` so a destructor cannot deadlock on that queued checkpoint. Closing the review dialog aborts a pending confirmation but does not unlink an in-flight recycle.
- **Evidence:** Success requires `psiNewlyCreated != NULL` plus a gone source path. Missing Recycle Bin evidence after the source disappears is `UnexpectedPermanentRemoval` and stops the remainder. Persist failure after a verified recycle is `Uncertain` and must not retry Shell. Restart rewrites leftover `Attempting` rows to `Uncertain`; a missing path is not guessed Recycled.
- **Bug prevented:** Linking Shell recycle into `spacelens.exe`, treating `DeleteFileW` / `SHFileOperation` as recycle, use-after-free of the progress sink, claiming physical space freed, holding a SQLite write txn across IFileOperation, and treating bookkeeping failure as license to recycle again.

## Duplicate content hashing (BCrypt, no-follow)

- **Why used:** Verified duplicates need a live full SHA-256 of file contents. The
  index only supplies same-size candidates. Sample fingerprints (size + first /
  mid / last 64 KiB) are a narrowing filter, never a publishable result.
- **Ownership:** `WindowsFileContentHasher` owns one BCrypt SHA-256 algorithm
  handle. Each `hash()` call owns a content `HANDLE` (`FILE_READ_DATA |
  FILE_READ_ATTRIBUTES`, `FILE_FLAG_OPEN_REPARSE_POINT |
  FILE_FLAG_SEQUENTIAL_SCAN`) and a 1 MiB reusable buffer. The file is not
  mapped whole.
- **Lifetime:** The handle stays open for the entire read. Metadata and identity
  are inspected on that handle before and after hashing; the path is re-opened
  to detect replacement. A size, last-write, or identity change yields
  `ChangedDuringRead` / `IdentityChanged` / `Missing` instead of a digest.
- **Hard links:** Live `CleanupIdentity` collapses aliases before hashing. One
  identity with two or more paths is `same_file_identity` and is not hashed.
  Mixed alias + independent copy hashes once per identity.
- **Bug prevented:** Following a reparse point into another file, treating
  same-size or sample-equal files as verified, hashing the same hard-linked
  object twice, and publishing a digest from a file that changed mid-read.

## Duplicate detection session

- **Why used:** Hashing large candidates must not freeze the GUI or mutate the
  analyzed tree.
- **Ownership:** `DuplicateFilesDialog` owns `DuplicateDetectionSession`. The
  session owns one `std::jthread`. Core `detectDuplicates` is Qt-free.
- **Lifetime:** One sequential worker. Progress and completion are queued to the
  GUI thread. Cancel keeps already-finalized groups and marks the result
  partial. Destruction requests stop and joins.
- **Bug prevented:** Hashing from paint/model code, one thread per file, SQL in
  Qt widgets, and treating cancellation as a completed verification.

## Shared core JSON

- **Why used:** Cleanup Plan and CLI both need deterministic UTF-8 JSON escaping; core must not include CLI headers.
- **Ownership:** `src/core/Json.*` is the single implementation. CLI wrappers delegate; Cleanup Plan does not keep a private escaper. `wideFromUtf8` lives here so MCP/CLI can turn tool arguments into `wstring` paths without a second codec.
- **Lifetime:** Redaction of `%USERPROFILE%` happens only while rendering text/JSON. Stored candidate paths are never rewritten.
- **Bug prevented:** Divergent escaping, core depending on CLI, and redaction mutating durable review state.

## Isolated MCP JSON parser

- **Why used:** There is no official C++ MCP SDK. The stdio adapter must parse NDJSON requests without pulling a third-party JSON library or teaching `spacelens_core` to parse untrusted input.
- **Ownership:** `src/mcp/JsonValue.*` is recursive-descent and used only by `spacelens-mcp` and MCP unit tests. Core still writes JSON; it does not parse it.
- **Lifetime:** One `JsonValue` tree per incoming line. `stringify()` is compact and never emits raw newlines (stdout is protocol-only).
- **Bug prevented:** Protocol banners on stdout, core growing an unused parser, and embedding newlines that would split an MCP message.

## MCP stdio reader vs analysis mutex

- **Why used:** `notifications/cancelled` must be applied while a live scan is blocked in `analyzeOverview`. A single-threaded stdin loop would swallow cancel until the tool returned.
- **Ownership:** `McpServer::runStdio` owns a reader thread, a line queue, and `m_currentStop`. Expensive tools serialize on `m_analysisMutex`. stdout writes stay on the processor thread.
- **Lifetime:** Reader exits on stdin EOF; the process then returns 0. Incoming lines over 1 MiB are discarded and reported as `-32700`.
- **Bug prevented:** Uncancellable multi-minute scans, interleaved analysis races, and a banner/log line corrupting the MCP stream.

## SQLite RAII (`SqliteDb` / `SqliteStmt` / `SqliteTxn`)

- **Why used:** The amalgamation C API is error-code oriented; RAII makes open/prepare/finalize/close exception-safe and keeps a single writer path obvious.
- **Ownership:** `SqliteDb` owns one `sqlite3*`. `SqliteStmt` owns one `sqlite3_stmt*` and must be destroyed before the DB is closed. `SqliteTxn` issues `BEGIN` and `COMMIT`/`ROLLBACK` on scope exit.
- **Lifetime:** Staging builds open `index.db.building`; after inserts complete, statements are finalized and the DB is closed **before** `publishIndexDatabase` renames files. Read queries open the published DB read-only for the duration of one status/query call.
- **Bug prevented:** SQLITE_BUSY / unable-to-close with live statements; leaked prepares; half-published indexes when an exception aborts mid-insert.

## Staging publish for index rebuilds

- **Why used:** A full rebuild must not destroy a good previous index if the new build fails or is cancelled.
- **Ownership:** `IndexBuilder` owns the staging store until publish. AppData paths come from `locateIndex` (FNV-1a root key under LocalAppData).
- **Lifetime:** Write staging → close → `MoveFile` live to `.bak` → move staging to live → delete `.bak`. On failure after moving live aside, restore from `.bak`. Cancel/fail before publish only deletes staging.
- **Bug prevented:** Users losing the last good index mid-rebuild; partial DBs left as the published path.

## Roots meta upsert (no CASCADE wipe)

- **Why used:** `entries.root_id` references `roots(id) ON DELETE CASCADE`.
- **Ownership:** One logical root row (`id = 1`) per V1 database.
- **Lifetime:** `writeRootMeta` uses `INSERT … ON CONFLICT(id) DO UPDATE`. Never `DELETE FROM roots` while entries exist.
- **Bug prevented:** A second meta write after inserts silently deleting every entry via CASCADE (observed during V1 development as “publish succeeded, query returned 0 rows”).

## Value types versus owning pointers

- **Why used:** Scan records, classifications, policies, activity summaries, and reclaim results are ordinary data and should have explicit copy/move semantics.
- **Ownership:** Prefer value types and containers for records. Use `std::unique_ptr` only when an object is exclusively owned, separately allocated, polymorphic, or has a lifetime that must be decoupled from its containing value. Do not introduce `shared_ptr` merely to connect tree records or UI selections.
- **Lifetime:** A value lives with its owning snapshot/container; an owning pointer must make the owner and destruction boundary obvious.
- **Bug prevented:** Accidental shared ownership, cycles, unclear destruction, and pointers that outlive the snapshot containing the data they reference.

## Immutable scan snapshots

- **Why used:** CLI, GUI, and analysis must reason about one consistent scan result instead of observing a tree while it is being built.
- **Ownership:** The scan coordinator owns the current `ScanResult`; readers borrow const access or receive a copied/moved published snapshot. Publish a completed or cancelled snapshot as a whole and replace it for the next scan.
- **Lifetime:** UI models and analysis results must not retain mutable references into a later scan. Refresh or translate selections when the published snapshot changes.
- **Bug prevented:** Data races, mixed-scan totals, invalid indices after tree rebuilds, and views that display records from a destroyed result.

## TOCTOU and future mutation gates

- **Why used:** A scan or index is evidence at T1; revalidation and any future action run against a different filesystem state at T2.
- **Ownership:** `CleanupCandidate` owns captured evidence and identity. `ICleanupMetadataReader` owns one no-follow metadata probe. A future mutation layer would own the action decision; analysis, the Cleanup Plan, and AI output never do.
- **Lifetime:** Explicit revalidation compares existence, type, identity, reparse, location policy, and direct size/write/attributes. Directory recursive evidence stays historical unless separately revalidated. Refresh Evidence replaces the captured baseline only. Before any future action, recheck again and reject stale or ambiguous candidates.
- **Bug prevented:** Acting on a replaced path, traversing a newly introduced junction, comparing recursive directory size to a handle size, deleting changed data, or treating Refresh Evidence / a plan export as authorization.

## Path normalization for policy and review keys

- **Why used:** Windows paths can vary by separator and trailing-slash spelling, while policy and duplicate-review checks need deterministic comparisons.
- **Ownership:** `normalizePathForPolicy` returns an owning normalized string. Policy normalization is for classification/comparison; preserve the user/display path separately when presentation matters.
- **Lifetime:** Normalize at policy boundaries and when deriving a case-insensitive review key. Do not store a non-owning view into temporary path text.
- **Bug prevented:** Bypassing protected-root checks with alternate separators, duplicate review entries for equivalent spellings, and dangling path views.

## Qt model/view ownership

- **Why used:** Qt parent-child ownership is useful for widgets and models, but it must not replace core data ownership.
- **Ownership:** A Qt parent owns its child `QObject`s. A view references its model; the model owns only its presentation state and either observes or receives a snapshot, while `DirectoryTree` remains owned by core/session state. Avoid raw owning pointers in item data.
- **Lifetime:** Destroy or detach the model before its referenced snapshot is released, and reset the view when a new snapshot is published.
- **Bug prevented:** Double deletion, views pointing at freed scan records, and accidental mutation of core data through UI pointers.

## Worker-to-GUI handoff

- **Why used:** Filesystem I/O and analysis must not block or touch the GUI thread.
- **Ownership:** The worker owns only its operation-local values and publishes progress/result messages. The GUI thread owns widgets and applies queued updates.
- **Lifetime:** Use queued delivery (`QMetaObject::invokeMethod(..., Qt::QueuedConnection)` or equivalent) and ensure session destruction requests stop and joins before session-owned state disappears.
- **Bug prevented:** Cross-thread widget access, event-loop races, callbacks into destroyed windows, and completion handlers reading partially published results.

## First-party MSVC warnings and `/analyze`

- **Why used:** Release Engineering V0.1 treats first-party core, CLI, MCP,
  and maintenance as a warnings-as-errors surface. SQLite, Qt headers, and
  generated moc/uic stay outside that gate.
- **Ownership:** `cmake/SpaceLensWarnings.cmake` applies `/W4 /permissive-`
  and optional `/WX` per target. `/analyze /analyze:external-` is a separate
  preset (`windows-analyze`) and is never combined with `/WX`, so analyzer
  C6xxx noise cannot fail a `/WX` build.
- **Lifetime:** `/WX` is the default for those first-party targets. `/analyze`
  is off unless `SPACELENS_MSVC_ANALYZE=ON`.
- **Bug prevented:** Shipping first-party C4530-class noise as “clean”, and
  failing CI on third-party or analyzer warnings that are not confirmed
  product defects.

## LastAccessTime is advisory

- **Why used:** Windows last-access updates can be disabled, delayed, or caused by scanners and backup tools.
- **Ownership:** Store LastAccessTime as optional/advisory metadata alongside write-based activity; it must not replace deterministic modification-time evidence.
- **Lifetime:** Analysis may mention the value when known, but missing or surprising access time must remain unknown evidence rather than being normalized into "unused".
- **Bug prevented:** Strong reclaim recommendations based only on an unreliable access timestamp and the false equation `old == unused == safe`.
