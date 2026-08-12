# SpaceLens Architecture

## Product shape

SpaceLens is **not** primarily a GUI application. It is a native storage-analysis
engine with multiple front ends:

```text
            spacelens_core
           /              \
          /                \
   spacelens (CLI)     SpaceLens (GUI)
   agent / scripts     Qt Widgets desktop
```

The **CLI is a first-class product interface**. AI coding agents (Claude Code,
Codex, OpenCode, etc.) and automation scripts are expected to drive SpaceLens
from the terminal using deterministic, machine-readable output.

The GUI is an optional interactive surface over the same core.

## Scope (current)

Phases covered here:

1. Functional scanner (enumeration, aggregation, Top-K, cancellation)
2. CLI queries (`scan`, `top`, `--json`, exit codes)
3. GUI shell wired to the same scanner

Deferred: AI inside the product, SQLite history, NTFS MFT fast path, duplicates,
persistent index / watch mode, automatic deletion.

## Layered architecture

```text
cli / gui
    ↓
app (GUI-only adapters, e.g. Qt ScanSession)
    ↓
core  (ScanEngine, DirectoryTree, TopK, queries, formatting helpers)
    ↓
platform/windows  (IFileEnumerator, FindHandle, Explorer helpers)
```

| Layer | Responsibility | Forbidden |
|-------|----------------|-----------|
| **core** | Scan algorithms, data model, Top-K, size formatting, JSON-friendly result shaping | Qt types, interactive prompts, stdout policy |
| **platform** | Win32 enumeration and OS integration | Qt, CLI argument parsing |
| **cli** | argv parsing, human/JSON rendering, exit codes, Ctrl+C → stop_token | GUI widgets |
| **gui/app** | Qt windows, threads↔signals bridge | Scan algorithms |

Dependency direction is strictly **upward only**: core never includes CLI or GUI headers.

## Targets

| CMake target | Kind | Output (Windows) | Links |
|--------------|------|------------------|-------|
| `spacelens_core` | static library | `spacelens_core.lib` | Win32 — **no Qt** |
| `spacelens` | CLI executable | `build/cli/spacelens.exe` (console) | `spacelens_core` only |
| `SpaceLens` | GUI executable | `build/gui/spacelens-gui.exe` | `spacelens_core` + Qt6 |
| `spacelens_tests` | unit tests | `spacelens_tests.exe` | `spacelens_core` (+ CLI helpers) |

**Windows case-insensitivity:** `spacelens.exe` and `SpaceLens.exe` are the same
path. CLI and GUI therefore use distinct output names/directories.

Building the CLI must not require a running GUI. Qt is required only when
`SPACELENS_BUILD_GUI=ON` (default ON if Qt is found).

## Core types and ownership

- **`DirectoryTree`** — owns all directory/file records for one scan; index-based parent/child links; reconstructs paths on demand.
- **`FileEntry` / `DirectoryNode`** — value records inside the tree.
- **`IFileEnumerator`** — platform-neutral listing; tests use fakes.
- **`ScanEngine`** — synchronous recursive scan; accepts `std::stop_token` and throttled progress callback; **no Qt**.
- **`ScanResult` / `ScanProgress` / `ScanOptions`** — plain C++ value types shared by CLI and GUI.
- **`TopKCollector`** — bounded min-heap, O(N log K).

GUI-only:

- **`ScanSession`** (Qt) — owns `std::jthread`, marshals progress/completion to the GUI thread.

CLI-only:

- argument parsing, stdout/stderr policy, process exit codes, console Ctrl+C → `std::stop_source`.

## CLI design principles (agent-oriented)

1. **No interactive prompts** unless a future flag explicitly requests them.
2. **stdout** = primary result (human table or JSON).
3. **stderr** = diagnostics, progress (optional), errors.
4. **`--json`** = deterministic machine output; raw `size_bytes`; stable field names.
5. **Exit codes** distinguish success, usage errors, inaccessible root, scan failure, cancellation.
6. Commands are small verbs (`scan`, `top`, …) so agents can chain queries.
7. Do **not** embed an LLM in the CLI; external agents supply reasoning.

### Initial commands

```text
spacelens scan <path> [--json]
spacelens top  <path> (--files|--dirs) [--limit N] [--json]
spacelens help
spacelens version
```

Future (not implemented yet): `--min-size`, `--ext`, `index`, `query`, watch mode.

### JSON contract (stable fields)

Common envelope:

```json
{
  "ok": true,
  "command": "top",
  "root": "C:\\Users",
  "files_scanned": 182391,
  "directories_scanned": 18432,
  "bytes_scanned": 1234567890,
  "elapsed_ms": 8241,
  "access_denied": 4,
  "reparse_skipped": 3,
  "other_errors": 0,
  "state": "completed",
  "results": [ { "path": "...", "size_bytes": 0 } ]
}
```

- Sizes in JSON are always integer **bytes** (`size_bytes`).
- Human mode may pretty-print with KB/MB/GB via `SizeFormatter`.
- Decorative text never appears on JSON stdout.

### Exit codes

| Code | Meaning |
|-----:|---------|
| 0 | Success (completed scan/query) |
| 2 | Invalid arguments / usage |
| 3 | Inaccessible or missing root path |
| 4 | Scan failed |
| 5 | Cancelled (Ctrl+C / stop) |
| 1 | Unexpected internal error |

## Memory model

Index-based tree; leaf names only; full paths reconstructed. Avoids `shared_ptr`
graphs and per-file absolute path duplication.

## Concurrency and cancellation

- Core scan is synchronous + `stop_token` (easy to test).
- GUI runs the engine on `std::jthread` via `ScanSession`.
- CLI runs the engine on the main thread (or a worker) and maps console cancel to `stop_source`.
- No global mutex held during filesystem I/O.
- Progress is throttled (~100 ms) so consumers are not flooded.

## Windows enumeration

`FindFirstFileExW` + `FIND_FIRST_EX_LARGE_FETCH`, RAII `FindHandle` (`FindClose`).
Default: **do not follow directory reparse points**. Access errors are non-fatal
and counted.

## Aggregation invariant

```text
recursiveSize(dir) =
    sum(direct file sizes)
  + sum(recursiveSize(child directories))
```

Root total equals the sum of every discovered file size exactly once (for fully
scanned reachable content).

## Future indexing (not implemented)

Architecture must allow:

```text
spacelens index C:\
spacelens query ...
spacelens index C:\ --watch
```

without rewriting CLI/GUI. That implies: keep query surfaces on top of
`ScanResult` / future `IndexStore` interfaces, not ad-hoc GUI models.

## Deferred

AI product features, SQLite snapshots, MFT scanner, duplicates, deletion UX,
persistent index. Keep interfaces replaceable; do not hard-wire GUI types into core.
