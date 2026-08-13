# Releasing SpaceLens

## Version

`project(VERSION 0.1.0)` in the root `CMakeLists.txt` is the source of truth.
The CLI `version` command and zip names must contain that string.

Do not invent a marketing version that disagrees with CMake.

## What a release candidate is

A local or CI-produced pair of unsigned zip archives plus `SHA256SUMS.txt`:

```text
spacelens-cli-v<version>-windows-x64.zip
spacelens-gui-v<version>-windows-x64.zip
SHA256SUMS.txt
```

These are verification artifacts. They are not a public distribution grant.

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

Current tree: no `LICENSE`, review record is `PENDING` /
`source_availability=MISSING`. Overall status is
`RELEASE_DISTRIBUTION_BLOCKED` with `LICENSE_DECISION_REQUIRED` and
`QT_SOURCE_AVAILABILITY_REQUIRED`.

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
.\scripts\package-release.ps1
```

`package-release.ps1` runs `cmake --install` per component, deploys Qt
with `windeployqt --no-compiler-runtime --no-system-d3d-compiler
--no-system-dxc-compiler --release`, copies package notices and (if
present) `LICENSE`, then runs `scripts/verify-package.ps1`. The
validator requires `platforms\qwindows.dll` and the Qt DLLs that
`dumpbin /dependents` reports on `spacelens-gui.exe` (`Qt6Core.dll`,
`Qt6Gui.dll`, `Qt6Widgets.dll`), forbids Qt in the CLI stage, forbids the CLI executable and
MSVC CRT DLLs in either zip, forbids Windows SDK `dxcompiler.dll` /
`dxil.dll` and the SDK-sized `d3dcompiler_47.dll`, writes SHA-256 sums,
extracts the CLI zip, and re-runs the safety script.

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
| Windows / CLI-only Latest | windows-latest | none | `SPACELENS_BUILD_GUI=OFF`, safety, 2k stress smoke |
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

If there is no `LICENSE`, the status is `LICENSE_DECISION_REQUIRED` and
no GitHub Release is created.

Releases are prerelease. Do not mark v0.1.0 as latest/production from this
workflow. Do not push a `v*` tag from an assistant session.

## Do not do from an assistant session

- Choose a license
- Set `review_status=PASS` or `source_availability=READY`
- Create `docs/QT_REDIST_REVIEWED.md` before corresponding source exists
- Push `v0.1.0` or any other release tag
- Publish a GitHub Release
- Commit Visual C++ runtime DLLs
- Generate or commit a certificate, `.pfx`, or private key
- Weaken `filesystem_mutation: false` so CI is easier
