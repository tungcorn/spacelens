# Changelog

All notable changes to SpaceLens are recorded here. The version source of truth
is `project(VERSION …)` in the root `CMakeLists.txt`.

## [Unreleased]

## [0.1.8] — 2026-08-23

https://github.com/tungcorn/spacelens/releases/tag/v0.1.8

<!-- BEGIN GENERATED CHANGELOG 0.1.8 -->
### Added

- zalo: enhance video previews, add direct open/play action, and unify English UI
- ui: add real-time scanning progress UI and metrics for Zalo storage review
- zalo: add interactive delete actions and scanning progress feedback
- zalo: implement fileNoise bounded identification probe and relocated root auto-discovery
- zalo: implement Zalo Storage Inspector and Human-Recognizable Content V1

### Fixed

- core: use dynamic buffer in RelativeCursor to resolve MSVC analyze C6262 stack warning
- zalo: fix clean file noise in-place update and support raw stream playback
- security: reinforce multi-layer safety validation and deduplication in Zalo deletion
<!-- END GENERATED CHANGELOG 0.1.8 -->

## [0.1.7] — 2026-08-16

https://github.com/tungcorn/spacelens/releases/tag/v0.1.7

<!-- BEGIN GENERATED CHANGELOG 0.1.7 -->
### Added

- assets: add project logo, hero banner, and app icon resources
<!-- END GENERATED CHANGELOG 0.1.7 -->

## [0.1.6] — 2026-08-16

https://github.com/tungcorn/spacelens/releases/tag/v0.1.6

<!-- BEGIN GENERATED CHANGELOG 0.1.6 -->
### Added

- reclaim: add physical reclaim intelligence

### Fixed

- ci: handle missing release tag safely
- reclaim: drop stale index candidates after live revalidation
<!-- END GENERATED CHANGELOG 0.1.6 -->

- Prepare treats a missing next tag (`git ls-remote` empty) as
  `tag_exists=false` and continues release preparation instead of
  crashing on an empty-array index.
- Physical reclaim intelligence: `spacelens reclaim-plan` reports exact
  host-byte reclaim evidence (allocated bytes, hard-link coverage,
  provider ownership) without executing cleanup. Actionable vs
  review-only stay separate; `--target-free` never selects review-only.
  Index schema 3 persists physical accounting. Migrated v2 indexes stay
  queryable but `reclaim-plan --source persistent_index` fail-closes
  unless `physical_accounting=1`. No MCP reclaim tool.
- Generic install docs no longer hardcode a current release version.
  README, MCP, and packaged README files describe the latest published
  archives and the current npm distribution instead of a stamped v0.1.4.
- Release Decide skips prepare on expected no-op pushes (docs/chore/pin,
  or no releasable commits). That is success, not a red workflow.
- Prepare treats multiline `project(VERSION)` as already-bumped instead
  of re-running `prepare-release` and failing.
- `ensure-ci-run.ps1` reuses a successful or in-progress CI run on the
  exact SHA and dispatches `ci.yml` only when one is missing. Recovery
  `workflow_dispatch` no longer needs a rescue PR.
- Pin commits do not dispatch another full CI/release cycle. Pin-only
  pushes run a cheap `Pin / Verify` gate.
- `wait-npm` retries `npm view` after Trusted Publishing so registry
  propagation is not a false failure.
- Push CI concurrency is per-SHA so a later docs/pin commit cannot
  cancel the six required checks on a bump SHA.
- Release prepare updates `docs/QT_SOURCE_OFFER.md` so the current
  unified archive matches the new version and the previous archive
  stays listed as historical. Older v0.1.3–v0.1.0 entries are kept.

## [0.1.5] — 2026-08-15

https://github.com/tungcorn/spacelens/releases/tag/v0.1.5

<!-- BEGIN GENERATED CHANGELOG 0.1.5 -->
### Added

- index: add indexed storage breakdown
<!-- END GENERATED CHANGELOG 0.1.5 -->

### Indexed storage breakdown

- `spacelens breakdown <root>` reports file-logical-byte mix by stored
  classification, extension (top-N plus exact `other`), and last-write
  age. Index-only. Directories never contribute. Optional `--under` and
  `--max-index-age-seconds` reuse the existing prefix and freshness
  gates. No MCP tool. No GUI chart.

### Release process

- Patch releases are prepared on `release/next` and published after that
  PR is merged to `main`. `docs:` / `test:` / `ci:` / `chore:` commits
  do not open a release by themselves.
- Release preparation does not edit `.github/workflows/**`. Dispatch
  version inputs stay required and have no version-specific default.

## [0.1.4] — 2026-08-15

Indexed intelligence is now more exact, scalable, and honest. v0.1.3
stays published and immutable.

https://github.com/tungcorn/spacelens/releases/tag/v0.1.4

### Storage intelligence

- Sibling and project-context classification for developer, build, and
  cache trees. Leaf-name-only `temp` / `tmp` / `cache` / `build` is no
  longer enough.
- Deterministic `opportunity_rank_v2` and category aggregation. High-
  confidence regenerable items ≥ 10 MB rank Moderate even when recent.
- `opportunities --classification` / MCP classification filter applies
  at index fetch, not after a truncated prefetch.

### Exact indexed intelligence

- Indexed `opportunities` returns the exact public top-N across the
  whole matching published index. Inclusion filters run before `LIMIT`.
- Drive-root `--under D:\` / MCP `under` matches descendants.

### Exact aggregate accounting

- Indexed `unique_review_bytes` is overlap-aware across the whole
  matching set. The historical 50,000-row aggregate ceiling is gone.
- Totals are exact logical review bytes on published index evidence,
  not guaranteed freed disk space. `unique_review_estimated` is true
  only on integer overflow.

### Snapshot freshness

- Indexed `overview` / `opportunities` / `query` / `index status`
  report published-snapshot age (`index.freshness`,
  `basis=published_snapshot`). Exact results are exact for that
  snapshot, not live filesystem truth.
- Optional `--max-index-age-seconds` / MCP `max_index_age_seconds`
  fails closed before expensive analysis. No default policy, no
  auto-refresh, no live fallback.

### Index discovery

- `spacelens index list --json` is a compact published-index catalog
  (root, schema, status, cheap counts, snapshot freshness).
  Deterministic order. Broken entries stay visible. Listing does not
  refresh, live-scan, hash, or migrate schema.

### Safety

- CLI and MCP remain `filesystem_mutation: false` / `read_only: true`.
- MCP still exposes six read-only tools. Human-authorized Recycle Bin
  maintenance remains GUI-only.

## [0.1.3] — 2026-08-14

AI-ready storage intelligence release. v0.1.2 stays published and
immutable.

https://github.com/tungcorn/spacelens/releases/tag/v0.1.3

### Added

- Persistent Content Hash Cache / Duplicate Detection V2: verified SHA-256
  digests may be reused from `%LOCALAPPDATA%\SpaceLens\hash-cache.db` when
  FileId128 + size + ChangeTime + FileUsn still match. Path is never the
  key. A false cache hit is a defect; insufficient evidence hashes again.
  Cache write failures do not fail the scan. CLI, MCP, and GUI share the
  same `spacelens_core` implementation.
  See [`docs/DUPLICATES.md`](docs/DUPLICATES.md).
- Read-only MCP adapter V1: `spacelens-mcp.exe` is a native stdio Model
  Context Protocol server over `spacelens_core`. Six tools
  (`storage_capabilities`, `storage_overview`, `storage_opportunities`,
  `storage_query`, `storage_duplicates`, `storage_index_status`) share
  `StorageAnalysis` with the CLI. Dual-era protocol `2026-07-28` /
  `2025-11-25`. No Qt, no `spacelens_maintenance`, no mutation tools, no
  automatic index refresh. See [`docs/MCP.md`](docs/MCP.md).
- Storage Intelligence / Agent Interface V1: CLI `overview` and `opportunities`
  compose one live scan (or one published index) into a bounded storage
  summary and ranked review opportunities. Core types live in
  `spacelens_core` (`StorageIntelligence`) and are reused by the MCP adapter.
  See [`docs/AGENT_INTERFACE.md`](docs/AGENT_INTERFACE.md).
- `query --under PATH` restricts indexed results to a subtree. Windows
  path prefixes now escape `\` / `%` / `_` before `LIKE ? ESCAPE '\'`, so
  a prefix such as `D:\Projects\app` matches descendants instead of
  treating every backslash as the LIKE escape character.
- `scripts/verify-agent-interface.ps1` generates a temp developer-workstation
  fixture and checks the agent workflow against the built CLI.

### Changed

- Public distribution now includes `spacelens-mcp.exe`. The unified zip
  is GUI + CLI + MCP + Qt. The `spacelens-cli-*` zip is the headless
  profile (CLI + MCP, no Qt). npm `@tungcorn/spacelens` exposes
  `spacelens`, `spacelens-gui`, and `spacelens-mcp`.
- `find --json` now includes classification, location safety, reclaimability,
  candidate strength, and explanation. Live scan/top/find JSON reports
  `"source": "live_scan"`. `find` `truncated` is true only when more matches
  existed than `--limit`.
- Indexed `opportunities` fetch regenerable classes (including
  `TemporaryData`, excluding `DownloadedAiModel`) so a large model pile
  cannot hide recent caches. Overview `truncated_directories` accounts for
  the scan root always being the largest directory.
- `capabilities --json` advertises `overview`, `opportunities`,
  `json_contract_version`, and storage-intelligence feature flags.
  `filesystem_mutation` remains `false`.
- GUI UX V1: Live Scan and Indexed are segmented workspaces (no sidebar).
  Page headers, typographic metrics, Filters popups, first-class palette-aware
  treemap, and a command hierarchy replace the previous equal-weight toolbar
  chrome. Cleanup Review confirmation remains **Move eligible files to Recycle
  Bin**. Analysis stays read-only. Launch the local GUI with `.\scripts\dev.ps1`.
- GUI UX V1.1: higher-contrast secondary text, compact workspace and discovery
  segmented controls, Live Scan / Largest Files data tables, a property
  inspector that omits no-value fields, clearer Indexed root and empty
  states, Cancel only while busy, and a status-bar Read-only analysis chip.
  No filesystem mutation.

## [0.1.2] — 2026-08-14

First normal (non-prerelease) GitHub Release. Version remains
semantically pre-1.0. v0.1.0 and v0.1.1 stay published and immutable.

https://github.com/tungcorn/spacelens/releases/tag/v0.1.2

### Added

- Release Automation V2: `.github/workflows/release.yml` is
  `workflow_dispatch` only. `publish=false` packages and hashes
  without a tag, GitHub Release, or npm dispatch. `publish=true` on
  `main` requires green required CI, creates an annotated `v*` tag at
  the verified SHA, and publishes a latest GitHub Release (`--latest`,
  no `--prerelease`, no `--draft`).
- `scripts/verify-release-preflight.ps1` refuses version mismatch,
  existing tags/releases, an already-published npm version, and
  incomplete required CI.
- npm Trusted Publishing for `@tungcorn/spacelens@0.1.2` stays in
  `npm-publish.yml`. The release job dispatches that workflow after the
  public GitHub assets exist. Observed hash must match SHA256SUMS and
  an independent SHA-256 (dispatch input, required while the pin is
  still 0.1.1). Hash mismatch is a hard stop. No `NPM_TOKEN`.

### Changed

- Human-authorized Recycle Bin maintenance is V2: durable operation IDs,
  Attempting-before-Shell checkpoints, crash/restart Uncertain
  reconciliation, a 2-minute prepared-plan expiry, and an
  inspection-only Maintenance History dialog. The mutation remains
  GUI-only file-to-Recycle-Bin.
- `@tungcorn/spacelens` package version is 0.1.2 and wraps this
  release's unified zip. `@tungcorn/spacelens@0.1.1` remains on the
  registry. `packaging/npm/release-pin.env` stays on the last
  published 0.1.1 hash until the v0.1.2 zip exists; CI then skips
  pack-from-release while the package is ahead of the pin.
- GitHub Releases created by this workflow are latest/current, not
  prerelease. Historical v0.1.0 and v0.1.1 Releases stay prerelease.

## [0.1.1] — 2026-08-14

Packaging/distribution patch. No scanner, index, or maintenance behavior
change.

https://github.com/tungcorn/spacelens/releases/tag/v0.1.1

### Changed

- The primary Windows archive is `spacelens-v0.1.1-windows-x64.zip` and
  contains both `spacelens-gui.exe` and the read-only `spacelens.exe`
  plus the existing Qt 6.8.3 runtime and license/source-offer files.
- `spacelens-cli-v0.1.1-windows-x64.zip` remains the optional
  CLI-only profile. Do not install it alongside the complete archive.
- No new GUI-only zip. The published v0.1.0 GUI-only asset is unchanged.
- Release publish attaches the unified zip (not `spacelens-gui-*.zip`)
  when the Qt review gate passes. SHA256SUMS still lists only attached
  archives.
- `docs/release-notes/` is the GitHub Release body source. Staged
  WinGet 1.12.0 manifests cover the complete `tungcorn.SpaceLens`
  identifier and the optional `tungcorn.SpaceLens.CLI` identifier.
  Public `winget install` is not advertised until the community source
  resolves those identifiers.

## [0.1.0] — 2026-08-13

First public unsigned prerelease.
https://github.com/tungcorn/spacelens/releases/tag/v0.1.0

### Changed

- Maintainer-selected project license is MIT (`LICENSE`,
  `Copyright (c) 2026 tungcorn`). Qt is not MIT.
- Qt 6.8.3 corresponding source is a maintainer-controlled written
  offer plus a pinned official archive SHA-256
  (`docs/QT_SOURCE_OFFER.md`, `packaging/qt-source/SOURCE_IDENTITY.txt`).
- `packaging/qt-redist-review.env` is `PASS` / `source_availability=READY`
  for this 6.8.3 shared kit.
- Staged CLI and GUI zips must include `LICENSE`. The GUI zip must
  include the source identity and written offer.
- Distribution gates remain independent: a non-empty `LICENSE` can
  unlock a CLI-only GitHub Release; the GUI zip attaches only when
  `scripts/verify-qt-redist-review.ps1 -RequirePass` succeeds. Presence
  of `docs/QT_REDIST_REVIEWED.md` is not a gate. A CLI-only Release
  checksum file lists only the uploaded zip.
- `windeployqt` uses `--no-system-d3d-compiler` and
  `--no-system-dxc-compiler` so Windows SDK D3D/DXC compilers are not
  staged.

### Added

- `docs/QT_REDIST_REVIEWED.md` — completed 6.8.3 shared-kit review.
- `docs/QT_REDIST_AUDIT.md` — shipped-module inventory.
- `scripts/verify-qt-redist-review.ps1`, `scripts/verify-package.ps1`,
  `scripts/verify-release-checksums.ps1`,
  `scripts/verify-distribution-selftest.ps1`,
  `scripts/fetch-qt-corresponding-source.ps1`.
- GUI package notices plus verbatim GNU LGPL-3.0 / GPL-3.0 texts.

### Fixed

- Incremental USN refresh treats 8.3 and long Win32 paths as the same root.
  Hosted-runner TEMP (`C:\Users\RUNNER~1\...`) no longer drops in-root
  records whose `GetFinalPathNameByHandle` form is `C:\Users\runneradmin\...`.
  Reconstructed live paths are rebased onto the indexed root spelling so
  stored rows match a full walk and same-window parent materialization
  can find the existing root entry.
- Debug GUI link no longer fails with `rc.exe` RC1109 (`manifest.res`).
  `spacelens-gui` is non-incremental so CMake `vs_link_exe` does not pass
  a mixed-slash `/fo .../manifest.res` to the resource compiler.

### Added

- CMake presets `windows-debug`, `windows-release`, `windows-cli-release`, and
  `windows-analyze`. Qt is supplied through `CMAKE_PREFIX_PATH`, never through
  committed presets.
- `cmake --install` components `SpaceLensCli` and `SpaceLensGui` for portable
  staging. Tests, PDBs, static libraries, and AppData state are not installed.
- First-party MSVC `/W4 /permissive-` and optional `/WX`. MSVC `/analyze` is
  isolated to core and CLI and is not combined with `/WX`.
- GitHub Actions CI on pull requests, `main`, and `workflow_dispatch`:
  Full Debug, Full Release, CLI-only Latest, and MSVC `/analyze`.
- Weekly Dependabot updates for `github-actions` only.
- `scripts/verify-cli-safety.ps1` — capabilities JSON, `filesystem_mutation:
  false`, destructive and safety-write verbs rejected.
- `scripts/package-release.ps1` — CLI and GUI zip archives plus `SHA256SUMS.txt`.
- `scripts/stress-v01.ps1` — generated temporary fixtures only (default ≥100k
  entries). Not run on every pull request.
- Release workflow on `v*.*.*` tags. Publish stays blocked until a license is
  chosen and Qt redistribution is maintainer-reviewed.
- `CONTRIBUTING.md`, `docs/RELEASING.md`, `docs/THIRD_PARTY.md`.

### Safety

- CLI still links `spacelens_core` only. Configure fails if the CLI grows a
  `spacelens_maintenance` link.
- CI does not weaken location policy, Recycle Bin gates, or the read-only CLI
  contract.
