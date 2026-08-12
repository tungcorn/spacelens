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

## Cleanup candidate ownership

- **Why used:** Cleanup Review is a planning queue, not a filesystem-operation owner.
- **Ownership:** `CleanupReview` owns a `std::vector<CleanupCandidate>` by value. A candidate owns its path and copied selection metadata; it does not own a file handle, directory, or permission to mutate the filesystem.
- **Lifetime:** A candidate remains valid until removed or the review queue is cleared. Its snapshot metadata is retained for future revalidation, not treated as current filesystem truth.
- **Bug prevented:** Review state accidentally becoming delete authority, dangling references to scan entries, and hidden resource ownership in a UI queue.

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

- **Why used:** A scan is evidence at T1; a future action may run against a different filesystem state at T2.
- **Ownership:** `CleanupCandidate` owns the path, item kind, size-at-selection, write time, attributes, and classification needed to request revalidation. A future mutation layer owns the revalidation decision, not the analysis layer or AI output.
- **Lifetime:** Before any future action, recheck existence, item kind, reparse status, protected-location policy, expected parent, and material size/write-time changes. Reject stale or ambiguous candidates.
- **Bug prevented:** Acting on a replaced path, traversing a newly introduced junction, deleting changed data, or trusting an old review selection as current authorization.

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

## LastAccessTime is advisory

- **Why used:** Windows last-access updates can be disabled, delayed, or caused by scanners and backup tools.
- **Ownership:** Store LastAccessTime as optional/advisory metadata alongside write-based activity; it must not replace deterministic modification-time evidence.
- **Lifetime:** Analysis may mention the value when known, but missing or surprising access time must remain unknown evidence rather than being normalized into "unused".
- **Bug prevented:** Strong reclaim recommendations based only on an unreliable access timestamp and the false equation `old == unused == safe`.
