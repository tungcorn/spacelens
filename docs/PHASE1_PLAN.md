# Phase 1 Plan

Phase 1 delivers a functional scanner MVP in ordered milestones. Each milestone is intended to correspond to one focused git commit.

## 1. `docs/bootstrap`

### Deliverable

Project documentation skeleton and initial repository conventions: architecture, C++ notes, performance template, Phase 1 plan, and an honest README.

### Acceptance criteria

- The five requested documentation files exist and describe the current scope without claiming implemented features.
- Architecture boundaries and deferred work are explicit.
- The build prerequisites and future roadmap are documented.
- No source code or dependency is introduced by this milestone.

## 2. `data model`

### Deliverable

Core data types for `FileEntry`, indexed `DirectoryTree`, scan status/progress values, and `ScanSession` ownership/state sufficient to represent one result.

### Acceptance criteria

- Directory and file ownership is explicit and does not use a `shared_ptr` tree.
- Parent/child relationships and path reconstruction are testable.
- `recursiveSize` has a documented and enforceable aggregation invariant.
- The model builds with the intended C++20 toolchain and has focused unit tests for basic tree construction and empty/non-empty cases.

## 3. `sync scanner + tests`

### Deliverable

A synchronous `ScanEngine` using the `IFileEnumerator` abstraction and the Windows enumerator implementation, plus deterministic tests using a fake enumerator.

### Acceptance criteria

- Files and directories are enumerated under a selected root.
- Recursive directory sizes are correct, including empty directories and nested trees.
- Reparse-point directories are not followed.
- Permission/access errors are non-fatal and represented in scan results/status.
- The engine can be tested without depending on a particular live disk layout.
- Largest-file selection produces correct results for ordinary and tie cases.

## 4. `async + cancellation + progress`

### Deliverable

Asynchronous scan execution with bounded workers, cooperative `stop_token` cancellation, session status transitions, and throttled/coalesced progress reporting.

### Acceptance criteria

- The UI/app thread remains usable while a scan runs.
- A cancellation request reaches workers and results in a clean cancelled session without accessing destroyed state.
- Worker count and queued work are bounded.
- No global lock is held during filesystem I/O.
- Progress is monotonic for a given scan where the chosen metric allows it and is not emitted once per filesystem entry without throttling.
- Completion, cancellation, and error paths are distinguishable and test-covered.

## 5. `UI tree + largest items + explorer/clipboard`

### Deliverable

Qt Widgets UI in `MainWindow` with scan controls/status, directory tree, selected-directory details, Top-K largest files, Explorer navigation, and clipboard copy.

### Acceptance criteria

- A user can choose a root, start/cancel a scan, and see progress and final status.
- Directory sizes are visible in the hierarchy and selected-directory content can be inspected.
- The largest-items view is populated from the scan result and remains bounded by K.
- Explorer launch and clipboard copy use valid reconstructed paths and handle failures without crashing.
- UI code does not perform Win32 enumeration or own core scan data through accidental raw pointers.

## 6. `Top-K polish + Release build validation`

### Deliverable

Final Phase 1 correctness/performance polish for Top-K ordering and ties, error/status presentation, and a validated Release build.

### Acceptance criteria

- Top-K output is deterministic, correctly ordered, and handles fewer than K files, equal sizes, and replacement boundaries.
- Representative permission, empty-directory, cancellation, and unavailable-root cases have defined behavior.
- Debug and Release configurations build successfully with the documented prerequisites.
- Focused tests and the complete available test suite pass.
- No unsupported performance claim is added; benchmark work is recorded separately in `PERFORMANCE.md`.
