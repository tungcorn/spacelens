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

The first archive is the complete product (GUI + read-only CLI +
read-only MCP + Qt runtime). The second is the headless profile
(CLI + MCP, no Qt, no GUI). The `spacelens-cli-*` filename is kept
for continuity with v0.1.2; from v0.1.3 it is not CLI-only.
There is no GUI-only zip for v0.1.1 and later. Historical
`spacelens-gui-v0.1.0-*.zip` on the v0.1.0 Release is immutable.
`spacelens-mcp.exe` is installed into both current archives and the
npm package.

These are verification artifacts. They are not a public GitHub Release
until the pending `release/next` PR is merged to `main` and
`release.yml` publishes (`publish` decided automatically, or
`workflow_dispatch` with `publish=true` for recovery).

## Independent CLI and GUI gates

CLI and GUI are separate artifacts. Qt approval is **not** required to
distribute a Qt-free CLI.

| Gate | Unlocks | Check |
|------|---------|--------|
| Maintainer-chosen root `LICENSE` (non-empty) | Headless zip (CLI + MCP) may be attached to a GitHub Release | File exists and is not whitespace |
| Structured Qt review PASS | Unified zip (GUI + CLI + MCP) may be attached | `scripts/verify-qt-redist-review.ps1 -RequirePass` |

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

`package-release.ps1` installs `SpaceLensCli` and `SpaceLensMcp` into
the headless stage and `SpaceLensGui`, `SpaceLensCli`, and
`SpaceLensMcp` into the unified stage, deploys Qt onto the unified
tree with `windeployqt --no-compiler-runtime --no-system-d3d-compiler
--no-system-dxc-compiler --release`, copies package notices, the MIT
`LICENSE`, and the Qt source offer/identity, then runs
`scripts/verify-package.ps1`. The validator requires `spacelens.exe`,
`spacelens-gui.exe`, and `spacelens-mcp.exe` plus
`platforms\qwindows.dll` and `Qt6Core.dll` / `Qt6Gui.dll` /
`Qt6Widgets.dll` in the unified stage, requires CLI + MCP and forbids
Qt and the GUI in the headless stage, forbids MSVC CRT DLLs in either
zip, forbids Windows SDK `dxcompiler.dll` / `dxil.dll` and the
SDK-sized `d3dcompiler_47.dll`, writes SHA-256 sums, verifies a full
unified+headless checksum set and a headless filtered set, extracts
both zips, and re-runs CLI and MCP safety/wire gates on the packaged
binaries.

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
| npm / Package | windows-2022 | none | templates; pack-from-release only when pin matches |
| Release / Policy | ubuntu-latest | none | releasable-commit policy, dry-run, workflow static checks |

Push and `workflow_dispatch` CI use a per-SHA concurrency group so a later
docs-only or pin commit cannot cancel the six required checks on a bump
SHA. Pull requests still cancel superseded PR runs. Default
`contents: read`. No secrets are exposed to pull requests. Third-party
actions are pinned to full SHAs.

`.github/workflows/quality.yml` is a manual 100k+ stress job. It is not a
required pull-request check.

## Normal release path

SpaceLens patches are **released automatically** on every push to `main` that
contains a releasable commit. **The push itself is the release approval.**
There is no separate pull request to merge.

1. Push `feat:` / `fix:` / `perf:` (or a mislabeled commit that touches
   `src/core`, `src/cli`, `src/mcp`, `src/app`, `src/ui`, or
   `src/platform`) to `main`.
2. `release.yml` **Decide** always succeeds or fails on a real defect.
   Expected no-ops (`docs:` / `ci:` / `chore:` / pin commits, or no
   releasable commits since the latest stable tag) set `prepare_needed=false`
   and **skip** the prepare job. That is a green workflow, not a red
   "nothing to do" failure.
3. When a release is needed, **`prepare`** waits for the six required CI
   checks on the pushed SHA, then calls `prepare-release.ps1` to bump
   version files, the written Qt source offer (current unified archive
   plus historical previous), CHANGELOG, and release notes, commits
   `chore(release): prepare SpaceLens vX.Y.Z` directly on `main`, and
   pushes. If `main` moved while preparing the push fails safely (no
   force-push). Already-prepared CMake (multiline `project(VERSION)`)
   is treated as idempotent, not as a failed bump.
4. Because a `GITHUB_TOKEN` push does not automatically trigger another
   workflow run, prepare calls `ensure-ci-run.ps1` on the exact bump SHA
   (reuses a successful or in-progress CI run; dispatches `ci.yml` only
   when none exists), verifies that run's `head_sha`, then dispatches
   `release.yml` with `version=X.Y.Z publish=true`. If `main` moved
   before either step, prepare fails safely and does not publish another
   SHA.
5. The dispatched run enters the publish pipeline. **Ensure CI on this
   SHA** makes `workflow_dispatch` recovery valid by itself: it reuses
   green checks or dispatches CI for that SHA (no rescue PR). Preflight
   then waits for the six required checks on **that exact bump SHA**,
   then the workflow builds, tests, packages, tags **that exact bump
   commit**, publishes the GitHub Release, dispatches npm Trusted
   Publishing (retries `npm view` until the registry is visible), and
   pins `release-pin.env` from the **public** unified zip. The pin
   commit does **not** dispatch another full CI or release cycle.
6. `docs:` / `test:` / `ci:` / `chore:` / `style:` / `build:` commits do
   not release by themselves. `refactor:` does not unless it touches a
   product path. Pin-only changes run Detect + `Pin / Verify` +
   Release / Policy.
7. Multiple product commits pushed together produce **one** patch release.
8. Generated notes live between `<!-- BEGIN GENERATED NOTES -->` markers.
   If a notes file has no markers, it is left alone.

Inspect locally without publishing:

```powershell
.\scripts\release-needed.ps1 -DryRun
.\scripts\decide-release.ps1
.\scripts\prepare-release.ps1 -DryRun
.\scripts\verify-release-policy.ps1
.\scripts\verify-release-automation.ps1
```

Do not run `prepare-release.ps1` without `-DryRun` directly on `main`.

## Git tags and GitHub Releases

googleapis/release-please is **not** used. It would own the tag and
GitHub Release and cannot express dual-zip packaging, npm OIDC from the
public zip, the public hash pin, or CMake/`#define` version files.

`.github/workflows/release.yml` runs on push to `main` and keeps
**manual `workflow_dispatch`** as recovery. It does not run on tag
push. Publication concurrency group: `spacelens-release`
(`cancel-in-progress: false`).

Dispatch inputs (recovery / dry-run):

| Input | Default | Meaning |
|-------|---------|---------|
| `version` | *(required, no default)* | Must equal CMake `project(VERSION …)` and `packaging/npm/package.json` |
| `publish` | `false` | Dry-run when false; create tag + GitHub Release + npm dispatch when true |

`prepare-release.ps1` does not edit `.github/workflows/**`.

On push to `main`, the `prepare` job detects releasable commits and
creates the version-bump commit automatically. The subsequent
`workflow_dispatch` run calls `decide-release.ps1` with
`publish=true`. On a plain `push` event, `decide-release.ps1`
publishes only when CMake equals the next patch **and** this commit is
the bump (parent CMake differs) or the tag already points here.
A later product commit that still has the next-patch CMake does not
start another pipeline. A pin commit does not match that rule.
Prerelease tags such as `v0.1.5-rc.1` are ignored.

Dry-run (`publish=false` via dispatch) builds, tests, packages, hashes,
and stages the npm tarball from **this run's** unified zip. It must not
create a tag, GitHub Release, or npm publication. Ordinary
non-release pushes to `main` skip the Windows package job.

A GitHub Release is created only when **all** of the following are true:

1. decide selected `publish=true` (auto next-patch or dispatch recovery)
2. the ref is `main`
3. `scripts/verify-release-preflight.ps1 -Publish -Wait` succeeds:
   version matches CMake and package.json, the tag does not point at
   another SHA, required CI check-runs on this SHA are success
   (`Windows / Full Debug`, `Windows / Full Release`,
   `Windows / CLI-only Latest`, `Quality / MSVC Analyze`, `npm / Package`,
   `Release / Policy`). An existing GitHub Release or npm version for
   this tag is verified (required assets, not draft/prerelease) rather
   than treated as a hard error, so a rerun does not duplicate them.
4. both distribution gates pass (`cli_eligible` and `gui_eligible`)

The workflow then creates an annotated tag at `$GITHUB_SHA` if needed,
and runs `gh release create --latest --verify-tag` only when the Release
does not already exist. It does **not** pass `--prerelease` or
`--draft`. It refuses to move an existing tag or replace an existing
Release. If the tag already points at this SHA and the Release does not
exist (tag pushed, create failed), retry is allowed and only the
Release is created.

The unified zip, headless zip (`spacelens-cli-*`), and `SHA256SUMS.txt`
are attached.
`docs/release-notes/<tag>.md` is the Release body when that file exists.

After npm is on the registry, `update-release-pin.ps1` downloads the
**public** unified zip, requires observed SHA-256 == `SHA256SUMS.txt`,
and commits
`chore(npm): pin pack-from-release to public vX.Y.Z hash` on `main`
after the tag. It does not hash a local rebuild or the CI staging
artifact. If the pin already matches, it does not create another commit.
The pin job does not dispatch `ci.yml`; publication already proved the
immutable public asset.

v0.1.5 is the current latest Release from this workflow. Historical
v0.1.0, v0.1.1, v0.1.2, v0.1.3, and v0.1.4 remain published and must not
be retagged, redrafted, or have their assets replaced.

`release-pr.yml` is **retired** in V2. The `release/next` branch and
any pending PR are no longer used. The `prepare` job in `release.yml`
replaces them entirely.

Partial failure is forward-only. Do not roll back a public tag:

- GitHub success / npm fail: leave the Release; retry only
  `npm-publish.yml` if the version is not on the registry.
- npm success / verification fail: report; forward-fix; do not delete
  the Release or unpublish npm.
- pin fail: the public release stands; retry the pin job.
- pin success / post-release CI fail: diagnose forward; do not move
  the tag.
- prepare push fail (main moved): re-push a new product commit or
  re-run the `prepare` job manually; do not force-push.

If auto-publish fails after the tag exists, recover with
`workflow_dispatch` on `release.yml` from `main` while `main` still
is the tagged SHA, or from the tag itself (preflight allows a
non-`main` ref only when that tag already points at this SHA). If
only npm is missing, dispatch `npm-publish.yml`. Do not retag.

If the `prepare` job pushed the bump commit but the dispatch to the
publish pipeline failed, run `release.yml` via `workflow_dispatch`
with the bump version and `publish=true` manually. Recovery dispatches
CI for that SHA when the six checks are missing. Do not open a
temporary PR to manufacture required checks.

## npm

Templates live in `packaging/npm/`. The package name is
`@tungcorn/spacelens`. `package.json` version must match CMake.

`packaging/npm/release-pin.env` records the **last published** unified
zip (currently v0.1.5,
`88d60be76cd18266a47e290eacf0cc6e9a5ad13c935561fc8595a319240f2b46`).
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

The tarball contains the native GUI, the read-only CLI, the read-only
MCP server, the Qt runtime, and license/source-offer files. There is
no `postinstall` download. Node launchers spawn
`native\spacelens.exe` / `native\spacelens-gui.exe` /
`native\spacelens-mcp.exe` with `shell: false` and do not rewrite
stdout. The MCP launcher must emit protocol-only stdout.

`@tungcorn/spacelens@0.1.5` is on the public npm registry. Historical
0.1.1–0.1.3 stay published. The root README advertises
`npm install -g @tungcorn/spacelens`. Do not retag or republish
0.1.0–0.1.5.

`release-pr.yml` no longer runs on push. `update-release-pr.ps1` is
retained for reference and manual use but is not called by CI.

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
