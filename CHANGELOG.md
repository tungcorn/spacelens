# Changelog

All notable changes to SpaceLens are recorded here. The version source of truth
is `project(VERSION …)` in the root `CMakeLists.txt`.

This project does not yet have a public release. Entries below describe the
in-tree 0.1.0 line.

## [Unreleased]

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

## [0.1.0] — unreleased

Current `project(VERSION)` is `0.1.0`. No git tag and no GitHub Release has
been published for this version.
