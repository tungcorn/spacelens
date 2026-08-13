# Changelog

All notable changes to SpaceLens are recorded here. The version source of truth
is `project(VERSION …)` in the root `CMakeLists.txt`.

This project does not yet have a public release. Entries below describe the
in-tree 0.1.0 line.

## [Unreleased]

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

## [0.1.0] — unreleased

Current `project(VERSION)` is `0.1.0`. No git tag and no GitHub Release has
been published for this version.
