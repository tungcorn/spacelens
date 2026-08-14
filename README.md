# SpaceLens

**A fast native C++ storage intelligence engine and desktop analyzer for humans, scripts, and AI agents.**

SpaceLens scans Windows storage and turns a filesystem snapshot into deterministic
size, activity, classification, location-policy, and read-only reclaim insights.
It helps people find, understand, inspect, and plan cleanup without granting an
agent permission to destroy data.

```text
spacelens_core         →  spacelens (CLI, read-only)
                       →  SpaceLens (Qt desktop analyzer)
spacelens_maintenance  →  GUI Recycle Bin adapter only
```

## Downloads

Unsigned Windows x64 prerelease: [SpaceLens v0.1.1](https://github.com/tungcorn/spacelens/releases/tag/v0.1.1)

| Asset | Use |
| --- | --- |
| `spacelens-v0.1.1-windows-x64.zip` | Recommended: desktop GUI + read-only CLI (Qt 6.8.3 runtime included) |
| `spacelens-cli-v0.1.1-windows-x64.zip` | Optional CLI-only profile |
| `SHA256SUMS.txt` | SHA-256 of the attached zip assets |

Do not install both archives together; both expose `spacelens`.
The earlier [v0.1.0](https://github.com/tungcorn/spacelens/releases/tag/v0.1.0)
GUI-only zip remains published and is not replaced.

The official Microsoft Visual C++ Redistributable (x64) is required.
Binaries are unsigned. Verify the zip hashes before use.

Developer / terminal-friendly install (Windows x64):

```text
npm install -g @tungcorn/spacelens
```

That installs the same published unified archive: desktop GUI + read-only
CLI + Qt 6.8.3 runtime. Then run `spacelens` or `spacelens-gui`. Node is
required only to install and launch; SpaceLens remains native C++. The
Visual C++ Redistributable (x64) is still required. Templates live in
[`packaging/npm/`](packaging/npm/).

WinGet identifiers are staged in [`packaging/winget/`](packaging/winget/):
`tungcorn.SpaceLens` is the complete product; `tungcorn.SpaceLens.CLI` is
the optional CLI-only profile. They are not advertised as public install
commands until they resolve from the official WinGet source.

## Status

- Core scanner: working (Win32 enumeration, logical-size aggregation, Top-K,
  cancellation, progress, and reparse-point policy)
- Storage intelligence: deterministic classification, location safety policy,
  cleanup-review values, and read-only reclaim analysis
- **Persistent Index V1+V2:** SQLite full-root index under AppData; CLI
  `index` / `index refresh` / `index status` / `index list` / `query`
  (no live query fallback; USN incremental when volume access allows)
- CLI: `scan`, `top`, `find`, `index*`, `query`, `duplicates`, `capabilities`,
  `help`, `version` with versioned JSON and `filesystem_mutation: false`
- GUI: Live Scan + **Indexed** storage discovery (presets, search, filters,
  breadcrumb navigation, storage overview, interactive squarified treemap,
  inspector, Explorer/copy, **Find Duplicates**, durable Cleanup Review V2,
  and human-authorized Recycle Bin maintenance)
- Not yet: permanent delete, directory recycle, restore from Recycle Bin,
  auto-refresh on query, journal creation, persistent hash cache, MFT initial
  scan, MCP, product AI, automatic deletion, or CLI/agent mutation

## Features

- Fast native recursive scan using logical file sizes
- Largest files and largest directories with bounded Top-K queries
- Deterministic, explainable storage classification and confidence
- Protected/Sensitive/Ordinary/Unknown location safety policy, including
  GUI-only user-declared ordinary roots (classification only — not safe to
  delete)
- Write-based file and descendant-based directory activity summaries
- Read-only reclaim analysis for human review prioritization
- Durable Cleanup Review V2: captured evidence, strong object identity,
  metadata-only revalidation, overlap-aware Cleanup Plan, and planning-only
  JSON
- Human-authorized Maintenance V2: GUI Recycle Bin only, after fresh preflight,
  confirmation, and a final identity/safety guard. Durable operation IDs and
  Attempting checkpoints reconcile crash/restart as Uncertain rather than
  guessing Recycled. History is inspection-only. Not permanent deletion.
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
  add snapshot-provenance items to durable Cleanup Review
- **Duplicate Detection V1:** index-backed same-size candidates, live identity
  collapse (hard links are aliases, not copies), sample narrowing, full
  SHA-256 verification, and planning-only add to Cleanup Review. Same size is
  not a duplicate. Potential redundant logical bytes are not guaranteed free
  space.
- Directory reparse points not followed by default; access errors are non-fatal
  and counted

**Safety contract:** the CLI is read-only by design. See
[`docs/SAFETY.md`](docs/SAFETY.md). Index design: [`docs/INDEX.md`](docs/INDEX.md).
Cleanup Review: [`docs/CLEANUP_REVIEW.md`](docs/CLEANUP_REVIEW.md).
Duplicates: [`docs/DUPLICATES.md`](docs/DUPLICATES.md).
Maintenance: [`docs/MAINTENANCE.md`](docs/MAINTENANCE.md).
Location safety: [`docs/LOCATION_SAFETY.md`](docs/LOCATION_SAFETY.md).
Measured baselines: [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).

## Build prerequisites

- Windows 10/11 x64
- MSVC (Visual Studio 2022/2026 C++ tools) with Windows 10/11 SDK
- CMake 3.21+
- Ninja
- Qt 6.8.3 (Widgets, Concurrent) — only for the GUI. Supply it with
  `CMAKE_PREFIX_PATH`; do not put a machine path in `CMakePresets.json`.

Presets: `windows-debug`, `windows-release`, `windows-cli-release`,
`windows-analyze`. See [`CONTRIBUTING.md`](CONTRIBUTING.md) and
[`docs/RELEASING.md`](docs/RELEASING.md).

## Build and run

```powershell
. .\scripts\dev-env.ps1
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

CLI only (no Qt required):

```powershell
cmake --preset windows-cli-release
cmake --build --preset windows-cli-release --target spacelens
.\build-cli-release\cli\spacelens.exe capabilities --json
.\build-cli-release\cli\spacelens.exe scan <folder> --json
.\build-cli-release\cli\spacelens.exe top <folder> --dirs --limit 20 --json
.\build-cli-release\cli\spacelens.exe index <folder> --json
.\build-cli-release\cli\spacelens.exe index refresh <folder> --json
.\build-cli-release\cli\spacelens.exe query <folder> --files --min-size 100MB --limit 20 --json
.\build-cli-release\cli\spacelens.exe duplicates <folder> --min-size 1MB --json
```

Point those commands at a folder you own. Do not use the SpaceLens source tree
as a destructive or stress target. Generated fixtures: `scripts/stress-v01.ps1`.

The CLI is read-only; `capabilities --json` must report
`filesystem_mutation: false`. CI re-checks that with
`scripts/verify-cli-safety.ps1`.

GUI (Windows output name is `spacelens-gui.exe` — not `spacelens.exe`, because
NTFS is case-insensitive and those names would collide):

```powershell
.\build-release\gui\spacelens-gui.exe
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
7. **Find Duplicates** live-verifies exact file-content copies for the selected
   root (planning only — no delete, link, or keep-one).
8. **Refresh Index** uses USN when available; otherwise the UI reports
   incremental unavailable and **Rebuild** remains explicit.

All Indexed queries and the treemap hit the persistent SQLite snapshot — they
do not rescan the analyzed tree. Sizes are **logical** (not physical size-on-disk).
See [`docs/INDEX.md`](docs/INDEX.md).

### Cleanup Review V2 (GUI)

1. Add candidates from **Live Scan** or **Indexed** discovery. SpaceLens
   persists captured evidence and best-effort object identity under
   `%LOCALAPPDATA%\SpaceLens\state.db`.
2. Close and reopen later — review state survives; indexes can be rebuilt
   without erasing it. Startup does not revalidate automatically.
3. Open **Cleanup Review** and run **Revalidate All** (cancellable,
   metadata-only). Missing, changed, denied, and failed records stay until
   you refresh evidence or remove them.
4. Inspect identity / direct-metadata / recursive-not-revalidated facts
   separately. A directory is never shown as simply Unchanged when recursive
   evidence was not revalidated.
5. **Refresh Evidence** replaces the captured baseline with current
   metadata. It never authorizes deletion.
6. **Copy Plan** or **Export JSON** (`plan_schema_version: 1`,
   `filesystem_mutation: false`). Optional `%USERPROFILE%` redaction is
   serialization-only.

See [`docs/CLEANUP_REVIEW.md`](docs/CLEANUP_REVIEW.md).

Tests:

```powershell
ctest --preset windows-release
.\build-release\tests\spacelens_tests.exe
```

Packaging (unsigned verification zips; not a public grant):

```powershell
.\scripts\package-release.ps1
```

SpaceLens-owned code is **MIT**. See [`LICENSE`](LICENSE). The
maintainer selected this license; an assistant did not choose it. Qt
6.8.3 remains under its own licenses (LGPL-3.0-only option for the
dynamic GUI runtime). Corresponding Qt source is offered by the
maintainer — [`docs/QT_SOURCE_OFFER.md`](docs/QT_SOURCE_OFFER.md).
Do not treat Qt as MIT. See
[`docs/RELEASING.md`](docs/RELEASING.md),
[`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md), and
[`docs/QT_REDIST_REVIEWED.md`](docs/QT_REDIST_REVIEWED.md).

See [`docs/CLI.md`](docs/CLI.md), [`docs/SAFETY.md`](docs/SAFETY.md),
[`docs/INDEX.md`](docs/INDEX.md), [`docs/CLEANUP_REVIEW.md`](docs/CLEANUP_REVIEW.md),
[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md),
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md),
and [`CONTRIBUTING.md`](CONTRIBUTING.md).

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
