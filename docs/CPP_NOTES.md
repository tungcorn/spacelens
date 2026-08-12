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

## `ScanSession` + `std::jthread` with Qt signals

- **Why used:** Keep the GUI responsive while `ScanEngine` walks the filesystem. `std::jthread` joins on destruction and exposes `std::stop_token` for cooperative cancel.
- **Ownership:** `MainWindow` owns `ScanSession` via `unique_ptr`. The session owns the worker thread and the latest `ScanResult` until the UI calls `takeResult()`.
- **Lifetime:** Starting a scan spawns one worker. Cancel or destruction calls `request_stop()` then `join()`. Progress/completion are delivered with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` so slots run on the GUI thread after the worker posts them.
- **Bug prevented:** Scanning on the GUI thread (frozen UI), use-after-free if the window closes mid-scan, and cross-thread Qt widget access. Progress is throttled inside `ScanEngine` so the event queue is not flooded.
