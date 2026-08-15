# SpaceLens Architecture

## Product shape

SpaceLens is a native C++ storage intelligence engine with multiple front ends. It
helps people and software **find, understand, inspect, and plan** storage cleanup;
it is not a filesystem mutation service.

```text
                 spacelens_core
                /      |       \
               /       |        \
      spacelens     spacelens-mcp    SpaceLens (GUI)
      CLI           stdio MCP        human inspection
      scripts       AI harnesses     + Recycle Bin
      read-only     read-only              |
                                     spacelens_maintenance
```

The **CLI is a first-class product interface**. The **MCP adapter**
(`spacelens-mcp.exe`) is the same read-only analysis for AI harnesses that
speak Model Context Protocol instead of argv. Neither receives a file delete
or file-move capability. The GUI is an optional interactive surface over the
same core snapshot and analysis types. See [`docs/GUI.md`](GUI.md) for the
Qt Widgets shell: two top-level workspaces (Live Scan, Indexed), system
palette, no permanent sidebar. Recycle Bin maintenance stays a confirmed GUI
workflow; CLI and MCP `filesystem_mutation` remain false. See
[`docs/MCP.md`](MCP.md).

## Scope: safety + storage intelligence milestone

This milestone defines the following product shape. Where executable wiring is
not yet complete, the sections below describe the intended architecture rather
than claiming a shipped feature.

- Native recursive scanning with logical file sizes, recursive aggregation,
  bounded Top-K queries, cancellation, and progress reporting.
- Immutable scan snapshots that can be inspected by the CLI, GUI, and analysis
  layers without each front end re-enumerating the filesystem.
- Deterministic, explainable **classification**: what an item appears to be,
  with category, confidence, rule identifier, and reason.
- Deterministic **location safety policy**: whether a path is Protected,
  Sensitive, Ordinary, or Unknown. Built-in `classifyLocation` is path-only.
  Effective safety may also use a human-declared ordinary root. Location
  policy is independent of classification and reclaimability. See
  [`docs/LOCATION_SAFETY.md`](LOCATION_SAFETY.md).
- Write-based **activity summaries** for files and directories, including
  descendant activity for directories and bounded age/count/byte summaries.
- **Read-only reclaim analysis** that combines size, activity evidence,
  classification, reclaimability, and location policy to prioritize human review.
- A durable **Cleanup Review V2** queue for planning and reporting. Captured
  evidence, object identity, and last validation live in
  `%LOCALAPPDATA%\SpaceLens\state.db` (`review_schema_version = 1`), independent
  of replaceable per-root indexes. See [`docs/CLEANUP_REVIEW.md`](CLEANUP_REVIEW.md).
- **Human-Authorized Maintenance V2**: the GUI may send eligible reviewed files
  to the Recycle Bin after fresh preflight, explicit confirmation, a final
  identity/safety guard, and durable Attempting/Recycled/Uncertain checkpoints.
  The Shell adapter lives in `spacelens_maintenance` and is not linked into the
  CLI. See [`docs/MAINTENANCE.md`](MAINTENANCE.md).
- An agent-safe CLI contract with `capabilities`, `scan`, `top`, `find`,
  `overview`, `opportunities`, `breakdown`, and `reclaim-plan` surfaces,
  filters, versioned JSON output, and explicit read-only capability
  reporting. See [`docs/AGENT_INTERFACE.md`](AGENT_INTERFACE.md).

The CLI wires `scan`, `top`, `find`, `overview`, `opportunities`,
`breakdown`, `reclaim-plan`, `capabilities`, `index`, `index refresh`,
`index status`, `index list`, `query`, `duplicates`, `help`, and
`version`.
Analysis filters and versioned JSON (`schema_version: 1`) are implemented. See
[`docs/CLI.md`](CLI.md), [`docs/INDEX.md`](INDEX.md), and
[`docs/DUPLICATES.md`](DUPLICATES.md).

**Persistent Index** stores a full SQLite snapshot under
`%LOCALAPPDATA%\SpaceLens\indexes\<rootKey>\index.db` for fast repeated
read-only queries (`index_schema_version: 3`). Rebuilds use a staging file and
atomic publish so a failed or cancelled rebuild never destroys the previous good
index. Queries report `source: persistent_index` and fail with exit code 6 when
no index exists — there is no silent live-scan fallback. Schema 3 persists
physical allocation and hard-link evidence; `reclaim-plan --source
persistent_index` requires meta `physical_accounting=1` (set only after a
full v3 finalize).

**Reclaim Intelligence V1** answers what can realistically reclaim host
bytes, how many, why, and what the trade-off is. Logical bytes, allocated
bytes, and `host_reclaim_bytes` stay distinct. Exact reclaim requires
complete hard-link coverage. Providers (Cargo, CMake, .NET, NuGet, npm,
pip) are filesystem-first and never execute cleanup. Existing
`reclaimability` / `candidate_strength` stay; the planner adds
`reclaim_confidence`, `reclaim_basis`, `actionability`, and `disruption`.
There is no MCP reclaim tool and no `safe_to_delete`.

**Incremental Index V2** adds a read-only USN Change Journal path:
`VolumeHandle` / `FileIdentity` / `UsnJournal` / `IndexRefresh`. Full builds
capture a checkpoint when the volume can be opened; `index refresh` applies
coalesced FRN deltas under the indexed root and advances the checkpoint only on
commit. Checkpoint `next_usn` is the driver READ continuation (or journal tip),
never `record.usn+1`; empty tails are not discontinuities. Journal
create/resize/delete is never used. Access denied or journal discontinuity
→ `full_rebuild_required` without guessing.

**Index Browser V2** (GUI Indexed workspace) maps UI state into core
`IndexDiscoveryPreset` + `IndexQuerySpec` (`searchText`, filters, sort,
`browsePath`). All SQL remains in `IndexQuery`; the Qt layer uses
`IndexHitTableModel` and never opens SQLite. Queries run off the UI thread;
refresh/rebuild stay on `IndexSession`. Open/Reveal validate live paths and
treat missing entries as stale snapshot, not index rewrites.

**Storage Overview + Treemap V1** adds hierarchical visual discovery for the
current indexed location:

```text
queryHierarchyChildren (core, parent_id children)
        ↓
StorageOverview (non-overlapping logical totals + child counts)
        ↓
prepareTreemapWeights + layoutSquarified (core, Qt-free)
        ↓
TreemapWidget (QPainter; cached layout; selection sync)
```

Child directory weights use recursive logical size; direct files use file size.
The “Other” bucket folds a long tail of tiny siblings for readability only.
Discovery presets continue to drive the result table independently of treemap
areas. Core remains free of Qt; paintEvent never runs SQL or filesystem scans.

**Cleanup Review V2** persists planning state independently of the index:

```text
Live Scan / Indexed add
        ↓
prepareCleanupCandidateForAdd (best-effort live identity)
        ↓
CleanupReviewController  (draft → persist → swap)
        ↓
%LOCALAPPDATA%\SpaceLens\state.db
        ↓
explicit Revalidate All  (metadata-only, cancellable, sequential)
        ↓
CleanupPlan  (overlap-aware unique size, planning-only JSON)
```

`MainWindow` owns the controller and `CleanupRevalidationSession`. Startup
loads review rows; it does not probe the filesystem. Core JSON helpers live
in `src/core/Json.*` so CLI and Cleanup Plan share escaping without core
depending on CLI.

**Duplicate Detection V2** uses the published index only as a same-size
accelerator. Live metadata, hard-link identity collapse, optional sample
fingerprints, and full SHA-256 verification run sequentially in core
(`Duplicates` / `DuplicateDetection`) with a BCrypt hasher that does not
follow the final reparse component. A derived SHA-256 cache
(`%LOCALAPPDATA%\SpaceLens\hash-cache.db`, never `state.db`) may reuse a
digest when FileId128 + size + ChangeTime + FileUsn still match. A false
cache hit is a correctness defect; insufficient evidence hashes again.
GUI work lives in `DuplicateDetectionSession` + `DuplicateFilesDialog`.
Adding a group to Cleanup Review sets `source = "duplicate_detection"` and
does not authorize deletion. See [`docs/DUPLICATES.md`](DUPLICATES.md).

Deferred work includes AI inside the product, auto-refresh on query, MFT-based
initial scan, watch mode, and any automatic deletion or movement. A future
mutation service, if approved, must be a separately permissioned surface
rather than an ordinary CLI verb.

## Layered architecture

```text
cli / gui
    ↓
app (GUI-only adapters, e.g. Qt ScanSession)
    ↓
core (scan snapshots, queries, classification, policy, activity, review analysis)
    ↓
platform/windows (IFileEnumerator, FindHandle, Explorer helpers)
```

| Layer | Responsibility | Forbidden |
|-------|----------------|-----------|
| **core** | Scan algorithms, owned data model, live queries, classification, location policy, activity summaries, read-only reclaim analysis, cleanup-review values, Cleanup Plan, shared JSON/UTF-8 helpers, size formatting, **persistent index**, **durable review state** (`state.db`), **derived hash cache** (`hash-cache.db`) | Qt types, interactive prompts, stdout policy, filesystem mutation of analyzed roots |
| **platform** | Win32 enumeration, volume/USN read-only helpers, file identity (FRN), metadata-only cleanup probes (`FILE_ID_INFO`), path/Explorer integration | Qt, CLI argument parsing, shell-command construction, journal mutation, following reparse points for identity |
| **cli** | argv parsing, capability declaration, human/JSON rendering, filters, exit codes, Ctrl+C → `stop_token`, index/refresh/query commands | GUI widgets, delete/move/execute commands |
| **gui/app** | Qt windows, models/views, review planning, `CleanupRevalidationSession`, threads↔signals bridge | Scan algorithms, direct ownership of Win32 enumeration |

Index storage is AppData-only metadata. Core may create/replace files under
`%LOCALAPPDATA%\SpaceLens\` (indexes, `state.db`, `hash-cache.db`); it must
never delete or move user content under a scanned root. `read_only` means
analyzed user files are not mutated. AppData implementation state is not a
mutation of the scanned tree.

Dependency direction is strictly **upward only**: core never includes CLI or GUI
headers. Analysis results are data; neither an AI explanation nor a UI label can
override the core location policy.

## Safety and storage-intelligence layers

The analysis pipeline keeps these concepts separate:

```text
scan snapshot at T1
        ↓
classification       what the item looks like
        +
location policy      where the item is and how cautious to be
        +
activity summary     deterministic write-based evidence
        ↓
read-only reclaim analysis   review priority, not permission
        ↓
Cleanup Review       human planning queue, not filesystem action
```

### Classification

Classification is deterministic and explainable. A `Classification` contains a
storage category, confidence, rule identifier, and reason. Typical categories
include build artifacts, dependency directories, package caches, IDE caches, log
or temporary data, application data, system data, user data, and Unknown.
Classification describes *what* data appears to be; it does not say that the data
may be deleted.

### Location safety policy

Location policy is deterministic, core-owned, and independent of AI output and
classification. The intended policy classes are:

| Class | Typical examples | Review posture |
|-------|------------------|----------------|
| **Protected** | Windows, Program Files, recovery/system volumes, drive roots | Do not manage deletion |
| **Sensitive** | User profile roots, AppData, critical application configuration | Extra warnings and conservative review |
| **Ordinary** | Typical project folders and downloaded content under user trees; also a specific user-declared root | Eligible for review workflows |
| **Unknown** | Unrecognized layouts, including many data-volume trees until declared | Conservative treatment |

Built-in classification never treats an arbitrary drive-letter path as Ordinary.
A GUI-only declaration may mark one specific root ordinary. That is not
permission to delete. Protected and Sensitive cannot be overridden.

`AppData` needs nuanced treatment; it is not a blanket deletion area or a blanket
bulk-protected area. This milestone uses location policy for warnings, filtering,
and candidate scoring only. It does not provide a protection override checkbox.

### Activity summary

A file uses its last-write time as its primary activity evidence. A directory is
summarized from the descendants observed during the scan rather than relying only
on the directory object's timestamp. The intended directory summary contains:

- newest descendant modification time;
- oldest descendant modification time when useful; and
- counts and logical bytes modified within 30, 90, 180, and 365 days relative to
  the analysis time.

The summary is aggregated bottom-up in the same pass as recursive sizes. Access
times may be retained as advisory metadata, but LastAccessTime is not proof of
use and cannot alone produce a Strong candidate. Incomplete or inaccessible
subtrees must remain visible as incomplete/unknown evidence rather than being
silently treated as inactive.

### Read-only reclaim analysis

Reclaim analysis is a read-only calculation for human review prioritization. A
candidate combines:

```text
logical size
  + write/descendant-write inactivity evidence
  + deterministic classification and reclaimability
  + deterministic location safety policy
```

The result may expose `reclaimability` (`LikelyRegenerable`,
`PossiblyRegenerable`, `Unknown`, or `NotApplicable`) and a candidate strength
such as `None`, `ReviewOnly`, `Moderate`, or `Strong`. Protected locations remain
non-actionable regardless of age or size. Old user data remains Review Only or
Unknown reclaimability; age alone is never a deletion decision.

No layer emits or stores `safe_to_delete`. That field is intentionally absent.

### Cleanup Review

`CleanupReview` is the in-memory value model. `CleanupReviewController` is the
durable facade: mutate a draft, persist the whole logical operation, then swap
memory only after commit. Default storage is
`%LOCALAPPDATA%\SpaceLens\state.db` (`review_schema_version = 1`), independent of
`indexes/*/index.db`.

A candidate keeps captured evidence, preferred `FILE_ID_INFO` identity (with an
explicit 64-bit file-index fallback), historical directory aggregate, and last
validation. Revalidation is metadata-only, explicit, sequential, and
cancellable; cancellation discards partial batches. A directory whose object
identity and direct metadata still match is
`DirectUnchangedRecursiveNotRevalidated`, never unqualified `Unchanged`.

`CleanupPlan` computes overlap-aware unique selected logical size and emits
deterministic UTF-8 text/JSON (`plan_schema_version: 1`, `planning_only`,
`read_only`, `filesystem_mutation: false`). Optional `%USERPROFILE%` redaction
is serialization-only. There is no filesystem delete or move operation.

See [`docs/CLEANUP_REVIEW.md`](CLEANUP_REVIEW.md).

## Targets

| CMake target | Kind | Output (Windows) | Links |
|--------------|------|------------------|-------|
| `spacelens_core` | static library | `spacelens_core.lib` | Win32 — **no Qt** |
| `spacelens_maintenance` | static library | `spacelens_maintenance.lib` | Recycle Bin adapter — **not linked by CLI** |
| `spacelens` | CLI executable | `build-*/cli/spacelens.exe` (console) | `spacelens_core` only |
| `SpaceLens` | GUI executable | `build-*/gui/spacelens-gui.exe` | `spacelens_core` + `spacelens_maintenance` + Qt6 |
| `spacelens_tests` | unit tests | `spacelens_tests.exe` | `spacelens_core` (+ CLI helpers) |
| `spacelens_maintenance_tests` | adapter tests | `spacelens_maintenance_tests.exe` | core + maintenance |

**Windows case-insensitivity:** `spacelens.exe` and `SpaceLens.exe` are the same
path. CLI and GUI therefore use distinct output names/directories.

Building the CLI must not require a running GUI. Qt is required only when
`SPACELENS_BUILD_GUI=ON`. `windows-cli-release` sets that option OFF.

Configure fails if target `spacelens` lists `spacelens_maintenance` in its
link libraries.

## Build, install, and CI

Presets (CMake 3.21 schema 3) live in `CMakePresets.json`. They contain no
user Qt paths. Local overrides belong in gitignored `CMakeUserPresets.json`
or in `CMAKE_PREFIX_PATH`.

| Preset | Output dir | GUI | Notes |
|--------|------------|-----|-------|
| `windows-debug` | `build-debug/` | ON | Full Debug |
| `windows-release` | `build-release/` | ON | Full Release |
| `windows-cli-release` | `build-cli-release/` | OFF | No Qt |
| `windows-analyze` | `build-analyze/` | OFF | MSVC `/analyze` on core/CLI |

Install is portable staging, not a Program Files installer:

```text
cmake --install <build> --prefix <stage> --component SpaceLensCli
cmake --install <build> --prefix <stage> --component SpaceLensMcp
cmake --install <build> --prefix <stage> --component SpaceLensGui
```

Only runtime executables are installed. Tests, PDBs, static libraries, and
`%LOCALAPPDATA%\SpaceLens` state are not. From v0.1.3 the unified zip
contains GUI + CLI + MCP; the `spacelens-cli-*` zip is the headless
CLI + MCP profile.

CI (`.github/workflows/ci.yml`) is the quality gate: Full Debug, Full Release
(with `scripts/verify-cli-safety.ps1` and zip staging), CLI-only Latest,
`/analyze`, and npm template checks. Pack-from-release runs only when
`package.json` matches the last-published pin in
`packaging/npm/release-pin.env`; otherwise that job skips packing.
Third-party actions are SHA-pinned. See [`docs/RELEASING.md`](RELEASING.md).

npm is an additional **installation channel**, not a Node SDK. The
`@tungcorn/spacelens` tarball embeds the same published Windows x64
GUI+CLI+MCP runtime. `bin/spacelens.js`, `bin/spacelens-gui.js`, and
`bin/spacelens-mcp.js` spawn the native executables with `shell: false`.
They do not add commands, rewrite `--json` output, or authorize
maintenance. The MCP launcher must keep stdout protocol-only.

## Core types and ownership

- **`DirectoryTree`** — owns all directory/file records for one scan; uses
  index-based parent/child links and reconstructs paths on demand.
- **`FileEntry` / `DirectoryNode`** — value records inside the tree.
- **`ScanResult` / `ScanProgress` / `ScanOptions`** — plain C++ value types shared
  by CLI and GUI.
- **`Classification`** — deterministic category/confidence/rule/reason value.
- **`LocationSafety`** — deterministic location-policy value.
- **`OrdinaryLocationPolicy` / `OrdinaryLocationDeclaration`** — effective
  location snapshot and persisted user-declared ordinary roots. Built-in
  `classifyLocation` stays unchanged.
- **`ReclaimCandidate` / `ReclaimQuery`** — read-only analysis values; neither
  carries mutation authority.
- **`CleanupReview` / `CleanupCandidate`** — value-owned planning records
  (captured/current evidence, identity, validation). Not owners of filesystem
  objects or mutation permission.
- **`CleanupReviewStore` / `CleanupReviewController`** — independent SQLite
  review state and transactional draft/persist/swap facade.
- **`CleanupPlan`** — pure overlap-aware planning transform and text/JSON
  export. Shared JSON helpers live in `core/Json`.
- **`ICleanupMetadataReader` / `CleanupRevalidation`** — Qt-free metadata
  probe and sequential cancellable compare.
- **`IFileEnumerator`** — platform-neutral listing; tests use fakes.
- **`ScanEngine`** — synchronous recursive scan; accepts `std::stop_token` and a
  throttled progress callback; contains no Qt.
- **`TopKCollector`** — bounded min-heap, O(N log K).

GUI-only:

- **`ScanSession`** — owns `std::jthread`, requests cooperative cancellation, and
  marshals progress/completion to the GUI thread.
- **`CleanupRevalidationSession`** — MainWindow-owned sequential metadata
  worker; applies only a completed validation batch; destruction requests stop
  and joins.
- A Qt model/view may reference the currently published snapshot, but it does
  not own or mutate the core scan records through raw pointers.

CLI-only:

- argument parsing, capability/schema declaration, filter validation,
  stdout/stderr policy, process exit codes, and console Ctrl+C →
  `std::stop_source`.

## CLI design principles (agent-oriented)

1. The CLI is read-only. It may enumerate and analyze; it must not delete, move,
   rename, purge, execute shell commands, or turn model output into commands.
2. No interactive prompts are required for query commands.
3. **stdout** is the primary result (human table or JSON).
4. **stderr** contains diagnostics, optional progress, and errors.
5. `--json` is deterministic machine output with stable field names and raw
   `size_bytes` values.
6. JSON output carries an integer `schema_version` so agents can validate the
   contract before consuming it.
7. `capabilities --json` explicitly reports `filesystem_mutation: false`.
8. Commands remain small verbs (`scan`, `top`, `find`) so agents can compose
   queries without an embedded LLM.

### Command contract

The milestone target surface is:

```text
spacelens capabilities [--json]
spacelens scan <path> [--json]
spacelens top  <path> (--files|--dirs) [--limit N] [--json]
spacelens find <path> [--min-size SIZE] [--older-than DAYS]
                    [--category CATEGORY] [--files|--dirs]
                    [--limit N] [--json]
spacelens help
spacelens version
```

`scan`, `top`, `help`, and `version` are the currently wired commands in the
checked-out CLI. `capabilities`, `find`, and the analysis filters are the
intended storage-intelligence extension; they must be treated as unavailable
until the executable advertises them.

`find` is a read-only query over a scan snapshot. It returns matching files and/or
directories with analysis fields such as classification, location safety,
activity evidence, reclaimability, candidate strength, and an explanation. It
does not add items to a review queue unless a separate human UI operation does
so, and it never mutates the filesystem.

### Filters

| Filter | Meaning |
|--------|---------|
| `--min-size SIZE` | Minimum logical size. JSON reports bytes; the input unit rules are below. |
| `--older-than DAYS` | Require write-based activity older than the requested age. Directory age uses newest descendant write activity. |
| `--category CATEGORY` | Restrict deterministic storage classification, for example `build-artifact`, `package-cache`, or `user-data`. |
| `--files` | Include files; with `top`, select largest files. |
| `--dirs` | Include directories; with `top`, select largest directories. |
| `--limit N` | Bound the number of returned results. |
| `--json` | Emit the machine-readable result on stdout. |

Filters constrain observation or analysis only. They do not change protected
location policy and do not turn a candidate into an authorization.

### Size units

Human size input uses binary powers of 1024:

```text
1 KB = 1 KiB = 1024 B
1 MB = 1024 KB
1 GB = 1024 MB
1 TB = 1024 GB
```

Accepted forms are `B`, `K`/`KB`/`KiB`, `M`/`MB`/`MiB`,
`G`/`GB`/`GiB`, and `T`/`TB`/`TiB`, with optional decimal fractions such as
`1.5 GB`. Decimal SI units based on 1000 are intentionally not accepted. JSON
always uses integer logical bytes in fields named `size_bytes`.

### JSON schema and capabilities

Machine-readable result envelopes use an integer `schema_version`. The milestone
contract starts at version `1`; incompatible field or semantic changes require a
new version. Additive fields must not change the meaning of existing fields.
Human text is not part of the JSON schema.

A capabilities response is intended to be shaped like this:

```json
{
  "schema_version": 1,
  "product": "spacelens",
  "read_only": true,
  "filesystem_mutation": false,
  "commands": ["capabilities", "scan", "top", "find", "help", "version"],
  "filters": ["--min-size", "--older-than", "--category", "--files", "--dirs", "--limit"],
  "size_units": "binary_1024"
}
```

Scan and query responses retain the existing common counters where applicable:
`ok`, `command`, `root`, `files_scanned`, `directories_scanned`,
`bytes_scanned`, `elapsed_ms`, `access_denied`, `reparse_skipped`,
`other_errors`, `state`, and command-specific `results`. Query result sizes are
always integer `size_bytes`. Analysis results may add classification, policy,
activity, and reclaim fields, but **must not add `safe_to_delete`**.

### Exit codes

| Code | Meaning |
|-----:|---------|
| 0 | Success, including a completed query with no matches |
| 1 | Unexpected internal error |
| 2 | Invalid arguments, unknown command, or unsupported filter |
| 3 | Inaccessible or missing root path |
| 4 | Scan or query failure |
| 5 | Cancelled by Ctrl+C / stop request |

## Mutation separation

The ordinary CLI remains a read-only surface for agents, scripts, and humans.
There are no delete, remove, `rm`, move, cleanup, purge, wipe, or execute
commands, hidden aliases, generic shell execution, or automatic conversion of AI
text into filesystem operations.

The GUI may present inspection and Cleanup Review planning. Review actions such as
open, reveal in Explorer, revalidate, refresh evidence, remove from review, clear
review, copy a plan, and export JSON are not filesystem mutation. Revalidation
reads attributes only and never follows a reparse point merely to make identity
comparison succeed. If destructive actions are ever approved, they belong in
a separately permissioned maintenance executable/service or a clearly isolated
human-authorized GUI path. That surface must revalidate every selected candidate
against the filesystem and deterministic policy before acting.

## Memory model

The scan tree uses index-based ownership; leaf names only; full paths are
reconstructed. This avoids `shared_ptr` graphs and per-file absolute path
duplication. Published snapshots should be treated as immutable by readers and
replaced as a whole for a new scan.

## Concurrency and cancellation

- Core scan is synchronous + `stop_token` and is straightforward to test.
- GUI runs the engine on `std::jthread` via `ScanSession`, and Cleanup Review
  revalidation on `CleanupRevalidationSession` (one probe at a time).
- CLI runs the engine on the main thread (or a worker) and maps console cancel to
  `stop_source`.
- No global mutex is held during filesystem I/O.
- Progress is throttled (approximately 100 ms) so consumers are not flooded.
- Worker code never touches Qt widgets. Results and progress cross into the GUI
  through queued delivery on the GUI thread.

## Windows enumeration

`FindFirstFileExW` + `FIND_FIRST_EX_LARGE_FETCH`, RAII `FindHandle` (`FindClose`).
Default: **do not follow directory reparse points**. Access errors are non-fatal
and counted. Future mutation code must never blindly traverse reparse points.

## Aggregation invariant

```text
recursiveSize(dir) =
    sum(direct file sizes)
  + sum(recursiveSize(child directories))
```

Root total equals the sum of every discovered file size exactly once for fully
scanned reachable content. Activity summaries use an analogous bottom-up
aggregation over the descendants observed in the same snapshot.

## Future indexing (not implemented)

Architecture must allow:

```text
spacelens index C:\
spacelens query ...
spacelens index C:\ --watch
```

without rewriting CLI/GUI. Query surfaces should sit on top of `ScanResult` and
future `IndexStore` interfaces rather than ad-hoc GUI models. An index must retain
the same safety distinctions and read-only agent boundary.

## Deferred

AI product features, MFT scanning, watch mode, deletion UX, and
mutation services remain deferred. Keep interfaces replaceable; do not hard-wire
GUI types or filesystem authority into core analysis.
