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
on the v0.1.0 Release is immutable. `spacelens-mcp.exe` is built from
source (`SPACELENS_BUILD_MCP`, default ON) but is **not** installed into
the current v0.1.2 zip or npm packages.

These are verification artifacts. They are not a public GitHub Release
until Release Automation V2 publishes from `main` (`publish=true`).

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
is a product policy for a joint release, not a technical reason to
hold the CLI behind Qt. A `publish=true` run refuses a partial GitHub
Release when either gate fails.

## Build and package locally

```powershell
. .\scripts\dev-env.ps1
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
.\build-release\tests\spacelens_tests.exe
.\scripts\verify-cli-safety.ps1 -CliPath .\build-release\cli\spacelens.exe
.\scripts\verify-mcp-safety.ps1 -McpPath .\build-release\mcp\spacelens-mcp.exe
.\scripts\verify-mcp-wire.ps1   -McpPath .\build-release\mcp\spacelens-mcp.exe
.\scripts\verify-mcp-parity.ps1 -CliPath .\build-release\cli\spacelens.exe `
                               -McpPath .\build-release\mcp\spacelens-mcp.exe
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

`.github/workflows/release.yml` is **manual `workflow_dispatch` only**.
It no longer runs on tag push. Inputs:

| Input | Default | Meaning |
|-------|---------|---------|
| `version` | `0.1.2` | Must equal CMake `project(VERSION …)` and `packaging/npm/package.json` |
| `publish` | `false` | Dry-run when false; create tag + GitHub Release + npm dispatch when true |

Dry-run (`publish=false`) builds, tests, packages, hashes, and stages the
npm tarball from **this run's** unified zip. It must not create a tag,
GitHub Release, or npm publication.

A GitHub Release is created only when **all** of the following are true:

1. `publish=true`
2. the ref is `main`
3. `scripts/verify-release-preflight.ps1 -Publish` succeeds: version
   matches CMake and package.json, the tag does not exist locally or on
   origin, the GitHub Release does not exist, npm does not already have
   that version, and required CI check-runs on this SHA are success
   (`Windows / Full Debug`, `Windows / Full Release`,
   `Windows / CLI-only Latest`, `Quality / MSVC Analyze`, `npm / Package`)
4. both distribution gates pass (`cli_eligible` and `gui_eligible`)

The workflow then creates an annotated tag at `$GITHUB_SHA`, pushes it,
and runs `gh release create --latest --verify-tag`. It does **not** pass
`--prerelease` or `--draft`. It refuses to move an existing tag or
replace an existing Release. If the tag already points at this SHA and
the Release does not exist (tag pushed, create failed), retry is
allowed and only the Release is created.

The unified zip, CLI-only zip, and `SHA256SUMS.txt` are attached.
`docs/release-notes/<tag>.md` is the Release body when that file exists.

v0.1.2 is the first latest/current Release from this workflow. Historical
v0.1.0 and v0.1.1 remain published prereleases and must not be retagged,
redrafted, or have their assets replaced.

If GitHub publication succeeds and npm later fails, leave the GitHub
Release alone. Retry only `npm-publish.yml`, and only if
`@tungcorn/spacelens@<version>` does not already exist.

## npm

Templates live in `packaging/npm/`. The package name is
`@tungcorn/spacelens`. `package.json` version must match CMake.

`packaging/npm/release-pin.env` records the **last published** unified
zip (currently v0.1.1,
`b4d4cb993bb53e1414c9fc156d9c29a5dca1b8640ac8d3b1229e5ff5a345793d`).
Do not point the pin at a version whose public zip does not exist yet.
A hash mismatch is a hard stop. Do not substitute a locally rebuilt zip
for a published pin.

While `package.json` is ahead of the pin, CI `npm / Package` validates
templates and skips pack-from-release. The release workflow packs npm
from **this run's** unified zip and the SHA-256 it just computed.

`.github/workflows/npm-publish.yml` stays a separate
`workflow_dispatch` workflow (Trusted Publisher is bound to that
filename). It downloads the **public** GitHub Release zip and
`SHA256SUMS.txt` and requires observed == sums == an independent
SHA-256. That independent digest is the `sha256` input, or the pin
when `package.json` still matches the pin. When the package is ahead
of the pin, `sha256` is required; do not derive expected from the
downloaded bytes. Hash mismatch is a hard stop. Publish runs only
when `publish=true` **and** the ref is `main`, using OIDC
(`id-token: write`). It does not publish on push to `main` or from
other branches. Do not add an `NPM_TOKEN` secret. Do not move
`npm publish` into `release.yml` (`workflow_call` would make npm
validate the caller filename).

```powershell
.\scripts\verify-npm-template.ps1
.\scripts\package-npm.ps1 -ZipPath <unified.zip> -ExpectedSha256 <sha256>
.\scripts\verify-npm-package.ps1
```

The tarball contains the native GUI, the read-only CLI, the Qt runtime,
and license/source-offer files. There is no `postinstall` download.
Node launchers spawn `native\spacelens.exe` / `native\spacelens-gui.exe`
with `shell: false` and do not rewrite stdout.

`@tungcorn/spacelens@0.1.1` is on the public npm registry. v0.1.2 is
published by dispatching `npm-publish.yml` after the v0.1.2 GitHub
Release exists. The root README advertises
`npm install -g @tungcorn/spacelens`. After a successful 0.1.2
publication, update `release-pin.env` to the new public hash so CI
pack-from-release works again.

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
- Move, delete, or recreate `v0.1.0`, `v0.1.1`, or any other published tag
- Replace published zip binaries for an existing tag
- Add an `NPM_TOKEN` secret or publish npm with a bypass-2FA token
- Mark a new Release as prerelease or draft from this workflow
- Commit Visual C++ runtime DLLs
- Generate or commit a certificate, `.pfx`, or private key
- Weaken `filesystem_mutation: false` so CI is easier
- Put the Qt source archive inside the GUI runtime zip
