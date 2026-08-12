# SpaceLens Architecture

## Scope and goals

This document describes the architecture for Phases 1–3. These phases establish a functional Windows disk-space analyzer: enumerate files and directories, aggregate directory sizes, present the results in a responsive Qt Widgets UI, support cancellation and progress reporting, and provide useful navigation and largest-item views. AI-assisted analysis, SQLite persistence, NTFS MFT access, and duplicate detection are deliberately outside this scope.

## Layered architecture

SpaceLens follows a one-way dependency flow:

```text
UI -> app -> core/analysis -> platform
```

- **UI** contains Qt Widgets, presentation state, user interaction, and view-model-style adapters. It does not enumerate Windows files or implement scan algorithms.
- **app** coordinates commands and application state. It creates scan sessions, connects progress/results to the UI, and owns the lifetime of long-running operations.
- **core/analysis** contains platform-neutral data structures and scan/aggregation algorithms. It depends on abstractions such as `IFileEnumerator`, not on Qt or Win32 details.
- **platform** contains Windows-specific enumeration and handle/error adapters. It translates Win32 results into core-facing types and policies.

Lower layers must not depend on higher layers. The core must remain testable with a fake `IFileEnumerator`; platform code must not leak Win32 handles or error conventions into UI code.

## Key types and ownership

- **`DirectoryTree`** owns the indexed directory and file records for one scan result. A directory node stores its parent index, child-directory indices, file indices, and aggregate `recursiveSize`.
- **`FileEntry`** is a value-like record owned by the tree or its file-record storage. It contains the file name/path components needed to reconstruct a path, size, and relevant metadata.
- **`IFileEnumerator`** is the core-facing enumeration abstraction. A platform implementation emits directory and file observations; tests can provide deterministic fakes.
- **`ScanEngine`** owns the scan algorithm and writes observations into a scan-owned tree/session. It performs traversal, aggregation, and largest-file selection without owning UI objects.
- **`ScanSession`** owns the state of one operation: the result tree, cancellation/progress state, scan status, and any bounded worker execution resources. The app owns the active session and releases it after completion or cancellation.

Ownership is explicit. Long-lived objects use value storage or `std::unique_ptr` where polymorphism or deferred construction is required. No scan result is retained by a global service.

## Memory model and paths

The directory hierarchy uses indices into stable storage rather than a graph of `shared_ptr` nodes. A node stores a parent index and child indices; files are similarly referenced by indices or compact records. This avoids reference cycles, makes ownership unambiguous, improves locality, and allows a complete scan result to be discarded in one operation.

Names and parent indices are sufficient to reconstruct a full path by walking from a node to the root and joining components in reverse order. The implementation should avoid storing a duplicated absolute path on every descendant unless a measured requirement justifies it. Temporary path strings are created at I/O boundaries or when requested by the UI.

## Concurrency and cancellation

Phase 1 begins with a synchronous engine so the traversal and invariants are easy to test. The asynchronous implementation uses bounded workers and `std::jthread`/`std::stop_token` (or a thin application-level equivalent if Qt thread integration is required). Work is partitioned only where it improves throughput; thread creation is bounded and there is no unbounded task queue.

Workers check the stop token at directory and batch boundaries. Cancellation is cooperative: the current Win32 operation is allowed to return, then the scan exits cleanly and reports a cancelled status. No global lock is held during filesystem I/O. Shared result mutations use narrow synchronization or ownership transfer, and progress updates are throttled/coalesced so the UI is not flooded by per-file signals.

## Windows enumeration

The Windows platform enumerator uses `FindFirstFileExW`/`FindNextFileW` and closes the search handle through an RAII wrapper. Directory traversal does not follow reparse points by default; reparse-point directories are recorded or skipped according to the scan policy, but are not recursively entered. Permission-denied and other per-entry access errors are non-fatal: the scan records the limitation when possible, skips the inaccessible branch, and continues with siblings. Fatal setup or invariant failures are reported to the session.

## Aggregation invariant

For every directory node, `recursiveSize` equals the sum of the sizes of files directly contained by that directory plus the `recursiveSize` of every scanned child directory. A directory is finalized only after its descendants have been processed, or its aggregate is updated through an equivalent bottom-up algorithm. Skipped or inaccessible content must not be silently counted as zero without preserving the scan's partial/incomplete status.

## Top-K largest files

The largest-file view maintains a bounded min-heap of at most `K` entries while scanning. Each candidate is inserted until the heap is full; thereafter it replaces the smallest retained candidate only when it is larger. This uses `O(N log K)` time and `O(K)` additional memory instead of sorting every file. The final heap is converted to a deterministic descending list for presentation, with a stable tie-breaker such as reconstructed path.

## UI structure

`MainWindow` is organized around a scan toolbar/status area and three complementary panels:

- a directory tree for hierarchical sizes and navigation;
- a details/list panel for the selected directory's files and child directories;
- a largest-items panel for the global Top-K files.

The UI displays scan state, progress, partial/error information, and cancellation controls. Explorer launch and clipboard copy operate on paths produced by the app/core layer; widgets do not derive paths by reaching into platform handles.

## Dependency direction rules

1. UI may depend on app-facing interfaces and Qt, but not directly on Win32 enumeration.
2. App may coordinate UI and core, but core types do not depend on app or UI types.
3. Core/analysis may depend on standard C++20 and narrow interfaces, but not Qt or Win32 headers.
4. Platform may depend on Win32 and standard C++20, and implements core interfaces.
5. Cross-layer data crosses through explicit value types, callbacks, or interfaces; do not expose framework-owned objects across boundaries.
6. Cancellation, errors, and progress are represented by policies/types that can be tested without a live UI.

## Deferred past Phase 3

The following are intentionally deferred: AI explanations or recommendations, SQLite history/index persistence, direct NTFS MFT scanning, duplicate-file analysis, content hashing at scale, cloud/network volumes as a specialized mode, and broad plugin/extensibility infrastructure. They should not shape Phase 1–3 interfaces beyond keeping the current boundaries replaceable.
