# Releasing SpaceLens

## Version

`project(VERSION 0.1.0)` in the root `CMakeLists.txt` is the source of truth.
The CLI `version` command and zip names must contain that string.

Do not invent a marketing version that disagrees with CMake.

## License

SpaceLens-owned code is MIT. SPDX: `MIT`. Text: root `LICENSE`.
Copyright: `Copyright (c) 2026 tungcorn`.

The maintainer selected MIT. An assistant did not choose this license.
MIT does not relicense Qt or SQLite.

Both staged zip archives must contain `LICENSE`.

## What a release candidate is

A local or CI-produced pair of unsigned zip archives plus `SHA256SUMS.txt`:

```text
spacelens-cli-v<version>-windows-x64.zip
spacelens-gui-v<version>-windows-x64.zip
SHA256SUMS.txt
```

These are verification artifacts. They are not a public GitHub Release
until a maintainer pushes a matching `v*` tag.

## Independent CLI and GUI gates

CLI and GUI are separate artifacts. Qt approval is **not** required to
distribute a Qt-free CLI.

| Gate | Unlocks | Check |
|------|---------|--------|
| Maintainer-chosen root `LICENSE` (non-empty) | CLI zip may be attached to a GitHub Release | File exists and is not whitespace |
| Structured Qt review PASS | GUI zip may be attached | `scripts/verify-qt-redist-review.ps1 -RequirePass` |

`RequirePass` succeeds only when `packaging/qt-redist-review.env` has
**all** of:

- `review_status=PASS`
- `qt_version` equal to the packaging pin (`6.8.3`)
- `linkage=shared`
- `source_availability=READY`

`Test-Path docs/QT_REDIST_REVIEWED.md` is **not** a gate. An empty
sentinel must not unlock anything. The release workflow never creates
`LICENSE`, the review env, or `QT_REDIST_REVIEWED.md`.

A Qt review bound to 6.8.3 is invalid for any other Qt version.

Combined public publication of **both** zips requires both gates. That
is a product policy for a joint prerelease, not a technical reason to
hold the CLI behind Qt.

## Build and package locally

```powershell
. .\scripts\dev-env.ps1
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
.\build-release\tests\spacelens_tests.exe
.\scripts\verify-cli-safety.ps1 -CliPath .\build-release\cli\spacelens.exe
.\scripts\verify-distribution-selftest.ps1
.\scripts\package-release.ps1
```

`package-release.ps1` runs `cmake --install` per component, deploys Qt
with `windeployqt --no-compiler-runtime --no-system-d3d-compiler
--no-system-dxc-compiler --release`, copies package notices, the MIT
`LICENSE`, and the Qt source offer/identity, then runs
`scripts/verify-package.ps1`. The validator requires
`platforms\qwindows.dll` and the Qt DLLs that `dumpbin /dependents`
reports on `spacelens-gui.exe` (`Qt6Core.dll`, `Qt6Gui.dll`,
`Qt6Widgets.dll`), forbids Qt in the CLI stage, forbids the CLI
executable and MSVC CRT DLLs in either zip, forbids Windows SDK
`dxcompiler.dll` / `dxil.dll` and the SDK-sized `d3dcompiler_47.dll`,
writes SHA-256 sums, verifies a full CLI+GUI checksum set and a
CLI-only filtered set, extracts the CLI zip, and re-runs the safety
script.

The Visual C++ Redistributable (x64) is a runtime prerequisite. Do not
copy compiler-runtime DLLs from a developer machine into the zip.

Binaries are unsigned. Do not generate a certificate or `.pfx`.
SpaceLens v0.1.0 remains unsigned unless a legitimate signing
certificate is provided separately.

## CI

`.github/workflows/ci.yml` runs on pull requests, pushes to `main`, and
`workflow_dispatch`:

| Job | Runner | Qt | Notes |
|-----|--------|----|--------|
| Windows / Full Debug | windows-2022 | 6.8.3 via aqtinstall | configure / build / ctest / logical tests |
| Windows / Full Release | windows-2022 | 6.8.3 | tests, CLI safety, package staging |
| Windows / CLI-only Latest | windows-latest | none | `SPACELENS_BUILD_GUI=OFF`, safety, 2k stress smoke, distribution selftest |
| Quality / MSVC Analyze | windows-2022 | none | `/analyze` on core + CLI |

Concurrency cancels superseded runs. Default `contents: read`. No secrets are
exposed to pull requests. Third-party actions are pinned to full SHAs.

`.github/workflows/quality.yml` is a manual 100k+ stress job. It is not a
required pull-request check.

## Git tags and GitHub Releases

`.github/workflows/release.yml` runs on `v*.*.*` tags and `workflow_dispatch`.

It builds, tests, safety-checks, and stages archives as workflow artifacts.

A GitHub Release is created only when **all** of the following are true:

1. the ref is a `v*.*.*` tag matching `v` + `CMAKE_PROJECT_VERSION`
2. a non-empty root `LICENSE` file exists (`cli_eligible`)

The GUI zip is attached only when `cli_eligible` **and** the structured
Qt review is PASS (`gui_eligible`). Otherwise the GUI zip stays on the
workflow run. The `SHA256SUMS.txt` attached to the GitHub Release lists
only the uploaded zips. The workflow artifact may still list both.

Releases are prerelease. Do not mark v0.1.0 as latest/production from this
workflow. Do not move or recreate an already-published `v*` tag.

The publish job uses `docs/release-notes/<tag>.md` as the GitHub Release
body when that file exists. Otherwise it writes a short unsigned-prerelease
fallback. Do not hardcode a one-line body in the workflow.

The existing `v0.1.0` Release title is `SpaceLens v0.1.0`. GitHub already
renders the Pre-release badge; do not add `(prerelease)` to the title.

## WinGet

Staged manifests live in `packaging/winget/`. Public identifiers:

- `tungcorn.SpaceLens` — desktop GUI
- `tungcorn.SpaceLens.CLI` — read-only CLI

Do not document `winget install --id …` in the README until
`winget show --id <id> --source winget` resolves. One package per
`microsoft/winget-pkgs` pull request.

## Do not do from an assistant session

- Change the maintainer-selected MIT license
- Move, delete, or recreate `v0.1.0` or any other published tag
- Replace published zip binaries for an existing tag
- Commit Visual C++ runtime DLLs
- Generate or commit a certificate, `.pfx`, or private key
- Weaken `filesystem_mutation: false` so CI is easier
- Put the Qt source archive inside the GUI runtime zip
