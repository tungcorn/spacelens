# SpaceLens

**A fast native C++ storage intelligence engine and desktop analyzer for humans, scripts, and AI agents.**

SpaceLens scans Windows storage and turns a filesystem snapshot into deterministic
size, activity, classification, location-policy, and read-only reclaim insights.
It helps people find, understand, inspect, and plan cleanup without granting an
agent permission to destroy data.

```text
spacelens_core  →  spacelens (CLI, read-only)
                →  SpaceLens (Qt desktop analyzer)
```

## Status

- Core scanner: working (Win32 enumeration, logical-size aggregation, Top-K,
  cancellation, progress, and reparse-point policy)
- Storage intelligence: deterministic classification, location safety policy,
  cleanup-review values, and read-only reclaim analysis
- **Persistent Index V1+V2:** SQLite full-root index under AppData; CLI
  `index` / `index refresh` / `index status` / `index list` / `query`
  (no live query fallback; USN incremental when volume access allows)
- CLI: `scan`, `top`, `find`, `index*`, `query`, `capabilities`, `help`,
  `version` with versioned JSON and `filesystem_mutation: false`
- GUI: Live Scan + **Indexed** storage discovery (presets, search, filters,
  breadcrumb navigation, storage overview, interactive squarified treemap,
  inspector, Explorer/copy, Cleanup Review planning only — no delete/move)
- Not yet: auto-refresh on query, journal creation, duplicates, MFT initial
  scan, MCP, product AI, automatic deletion, or automatic movement

## Features

- Fast native recursive scan using logical file sizes
- Largest files and largest directories with bounded Top-K queries
- Deterministic, explainable storage classification and confidence
- Protected/Sensitive/Ordinary/Unknown location safety policy
- Write-based file and descendant-based directory activity summaries
- Read-only reclaim analysis for human review prioritization
- Cleanup Review planning data with no delete or move operation
- Persistent SQLite index for fast repeated filtered queries
- Optional USN-based incremental refresh (`index refresh`) — read-only journal
  access; full rebuild required on discontinuity or access denied
- Read-only CLI with human and machine-readable (`--json`) output for
  humans, scripts, and AI agents
- Optional Qt desktop UI with live progress, cancel, Explorer, and clipboard
  actions
- **Index Browser V2 + Storage Overview / Treemap V1:** discover largest /
  old-and-large / developer / reclaim candidates from a published index without
  rescanning; search name/path/ext; storage overview with non-overlapping
  logical-size metrics; interactive squarified treemap of immediate children
  (with “Other” aggregation); drill into folders via treemap or breadcrumbs;
  add snapshot-provenance items to Cleanup Review
- Directory reparse points not followed by default; access errors are non-fatal
  and counted

**Safety contract:** the CLI is read-only by design. See
[`docs/SAFETY.md`](docs/SAFETY.md). Index design: [`docs/INDEX.md`](docs/INDEX.md).
Measured baselines: [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).

## Build prerequisites

- Windows 10/11 x64
- MSVC (Visual Studio 2022/2026 C++ tools) with Windows 10/11 SDK
- CMake 3.21+
- Ninja (recommended) or MSBuild
- Qt 6.8+ (Widgets, Concurrent) — e.g. `D:\Qt\6.8.3\msvc2022_64`

See [`docs/SURVEY.md`](docs/SURVEY.md) for the environment used during bootstrap.

## Build and run

```powershell
. .\scripts\dev-env.ps1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CLI only (no Qt required):

```powershell
cmake -S . -B build-cli -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPACELENS_BUILD_GUI=OFF
cmake --build build-cli --target spacelens
.\build-cli\cli\spacelens.exe scan C:\Users --json
.\build-cli\cli\spacelens.exe top C:\Users --dirs --limit 20 --json
.\build-cli\cli\spacelens.exe index C:\Users --json
.\build-cli\cli\spacelens.exe index refresh C:\Users --json
.\build-cli\cli\spacelens.exe query C:\Users --files --min-size 100MB --limit 20 --json
```

The CLI is read-only; capability discovery:

```powershell
.\build-cli\cli\spacelens.exe capabilities --json
```


GUI (Windows output name is `spacelens-gui.exe` — not `spacelens.exe`, because
NTFS is case-insensitive and those names would collide):

```powershell
.\build\gui\spacelens-gui.exe
```

### Indexed storage discovery (GUI)

1. **Index Folder…** (or CLI `index <root>`) to publish a snapshot under AppData.
2. Open the **Indexed** tab and select a root — header shows age, freshness,
   file/folder counts, and **indexed logical size**.
3. Read the **storage overview** and **treemap** for the current location
   (immediate children by logical size; double-click a folder to drill down).
4. Pick a discovery mode: **Largest**, **Old & Large**, **Developer Storage**,
   or **Reclaim Candidates** (or **Custom** filters) for the result table.
5. Optionally search (`Ctrl+F`), tighten min size / activity / classification,
   then inspect a row or treemap cell.
6. **Open** / **Show in Explorer** / **Copy Path**, or **Add to Cleanup Review**
   (planning only).
7. **Refresh Index** uses USN when available; otherwise the UI reports
   incremental unavailable and **Rebuild** remains explicit.

All Indexed queries and the treemap hit the persistent SQLite snapshot — they
do not rescan the analyzed tree. Sizes are **logical** (not physical size-on-disk).
See [`docs/INDEX.md`](docs/INDEX.md).

Tests:

```powershell
ctest --test-dir build --output-on-failure
```

See [`docs/CLI.md`](docs/CLI.md), [`docs/SAFETY.md`](docs/SAFETY.md),
[`docs/INDEX.md`](docs/INDEX.md), [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md),
and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Architecture

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the product shape,
layer boundaries, immutable scan snapshots, ownership model, safety and storage-
intelligence layers, CLI contract, concurrency policy, and deferred scope. The
ordered Phase 1 implementation checklist is in
[`docs/PHASE1_PLAN.md`](docs/PHASE1_PLAN.md).

## Roadmap

- **Phase 1:** Functional scanner MVP, indexed data model, aggregation,
  cancellation/progress, Qt tree UI, largest files, Explorer and clipboard actions.
- **Phase 2:** Usability and analysis improvements around the scanned result,
  error visibility, navigation, and measured performance work.
- **Phase 3:** A more complete scanning and presentation foundation with hardened
  platform behavior and maintainable extension points.
- **Phase 4:** Persistent scan history and related storage-backed features,
  subject to a separately defined design.
- **Phase 5:** Advanced filesystem analysis such as duplicate detection and
  specialized indexing, subject to explicit scope and validation.
- **Phase 6:** Optional intelligent assistance and other future capabilities,
  evaluated after the core product is reliable.

No performance claim is made until reproducible measurements are recorded in
[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).
