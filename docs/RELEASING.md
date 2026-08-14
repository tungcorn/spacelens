# Releasing SpaceLens

## Version

`project(VERSION …)` in the root `CMakeLists.txt` is the source of truth.
The CLI `version` command and zip names must contain that string.

Do not invent a marketing version that disagrees with CMake.

## License

SpaceLens-owned code is MIT. SPDX: `MIT`. Text: root `LICENSE`.
Copyright: `Copyright (c) 2026 tungcorn`.

The maintainer selected MIT. An assistant did not choose this license.
MIT does not relicense Qt or SQLite.

Both staged zip archives must contain `LICENSE`. The unified archive
must also contain the Qt license texts, source identity, and written
offer.

## What a release candidate is

A local or CI-produced pair of unsigned zip archives plus `SHA256SUMS.txt`:

```text
spacelens-v<version>-windows-x64.zip
spacelens-cli-v<version>-windows-x64.zip
SHA256SUMS.txt
```

The first archive is the complete product (GUI + read-only CLI + Qt
runtime). The second is the optional CLI-only profile. There is no
GUI-only zip for v0.1.1 and later. Historical `spacelens-gui-v0.1.0-*.zip`
on the v0.1.0 Release is immutable.

These are verification artifacts. They are not a public GitHub Release
until a maintainer pushes a matching `v*` tag.

## Independent CLI and GUI gates

CLI and GUI are separate artifacts. Qt approval is **not** required to
distribute a Qt-free CLI.

| Gate | Unlocks | Check |
|------|---------|--------|
| Maintainer-chosen root `LICENSE` (non-empty) | CLI-only zip may be attached to a GitHub Release | File exists and is not whitespace |
| Structured Qt review PASS | Unified GUI+CLI zip may be attached | `scripts/verify-qt-redist-review.ps1 -RequirePass` |

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

`package-release.ps1` installs the CLI component into the CLI-only
stage and both `SpaceLensGui` and `SpaceLensCli` into the unified
stage, deploys Qt onto the unified tree with `windeployqt
--no-compiler-runtime --no-system-d3d-compiler
--no-system-dxc-compiler --release`, copies package notices, the MIT
`LICENSE`, and the Qt source offer/identity, then runs
`scripts/verify-package.ps1`. The validator requires both
`spacelens.exe` and `spacelens-gui.exe` plus `platforms\qwindows.dll`
and `Qt6Core.dll` / `Qt6Gui.dll` / `Qt6Widgets.dll` in the unified
stage, forbids Qt and the GUI in the CLI-only stage, forbids MSVC CRT
DLLs in either zip, forbids Windows SDK `dxcompiler.dll` / `dxil.dll`
and the SDK-sized `d3dcompiler_47.dll`, writes SHA-256 sums, verifies
a full unified+CLI checksum set and a CLI-only filtered set, extracts
both zips, and re-runs the safety script on each `spacelens.exe`.

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

The unified zip is attached only when `cli_eligible` **and** the
structured Qt review is PASS (`gui_eligible`). Otherwise that zip stays
on the workflow run. The `SHA256SUMS.txt` attached to the GitHub
Release lists only the uploaded zips. The workflow artifact may still
list both.

Releases are prerelease. Do not mark v0.1.0 as latest/production from this
workflow. Do not move or recreate an already-published `v*` tag.

The publish job uses `docs/release-notes/<tag>.md` as the GitHub Release
body when that file exists. Otherwise it writes a short unsigned-prerelease
fallback. Do not hardcode a one-line body in the workflow.

The existing `v0.1.0` Release title is `SpaceLens v0.1.0`. GitHub already
renders the Pre-release badge; do not add `(prerelease)` to the title.

## npm

Templates live in `packaging/npm/`. The package name is
`@tungcorn/spacelens` version `0.1.1`. It distributes the **published**
unified archive `spacelens-v0.1.1-windows-x64.zip` after verifying
SHA-256
`b4d4cb993bb53e1414c9fc156d9c29a5dca1b8640ac8d3b1229e5ff5a345793d`.
A hash mismatch is a hard stop. Do not substitute a locally rebuilt zip.

```powershell
.\scripts\verify-npm-template.ps1
.\scripts\package-npm.ps1
.\scripts\verify-npm-package.ps1
```

The tarball contains the native GUI, the read-only CLI, the Qt runtime,
and license/source-offer files. There is no `postinstall` download.
Node launchers spawn `native\spacelens.exe` / `native\spacelens-gui.exe`
with `shell: false` and do not rewrite stdout.

`.github/workflows/npm-publish.yml` is **manual dispatch only**. Pack
always runs; publish runs only when the `publish` input is true **and**
the ref is `main`, after the same validation, using npm Trusted
Publishing (OIDC, `id-token: write`). It does not publish on push to
`main` or from other branches. Do not add an `NPM_TOKEN` secret.

`@tungcorn/spacelens@0.1.1` is on the public npm registry. The root
README advertises `npm install -g @tungcorn/spacelens`. Subsequent
publishes use this workflow after the maintainer binds Trusted Publisher
on npmjs.com (GitHub Actions, owner `tungcorn`, repository `spacelens`,
workflow filename `npm-publish.yml`, allowed action `npm publish`).
Saving that form is not a live OIDC test. Do not add an `NPM_TOKEN`
secret.

npm uninstall must not delete `%LOCALAPPDATA%\SpaceLens\`.

## WinGet

Staged manifests live in `packaging/winget/`. Public identifiers:

- `tungcorn.SpaceLens` — complete product (GUI + read-only CLI)
- `tungcorn.SpaceLens.CLI` — optional CLI-only profile

Do not install both at once; both expose `spacelens`. Do not document
`winget install --id …` in the README until
`winget show --id <id> --source winget` resolves. One package per
`microsoft/winget-pkgs` pull request. Do not submit installer URLs
until the matching GitHub Release assets exist.

## Do not do from an assistant session

- Change the maintainer-selected MIT license
- Move, delete, or recreate `v0.1.0` or any other published tag
- Replace published zip binaries for an existing tag
- Commit Visual C++ runtime DLLs
- Generate or commit a certificate, `.pfx`, or private key
- Weaken `filesystem_mutation: false` so CI is easier
- Put the Qt source archive inside the GUI runtime zip
