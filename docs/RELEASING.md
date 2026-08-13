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

`package-release.ps1` runs `cmake --install` per component, deploys Qt with
`windeployqt --no-compiler-runtime --release`, requires
`platforms\qwindows.dll`, forbids Qt in the CLI stage, forbids the CLI
executable in the GUI stage, writes SHA-256 sums, extracts the CLI zip, and
re-runs the safety script.

The Visual C++ Redistributable (x64) is a runtime prerequisite. Do not copy
compiler-runtime DLLs from a developer machine into the zip.

Binaries are unsigned. Do not generate a certificate or `.pfx`.

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
2. a root `LICENSE` file exists
3. `docs/QT_REDIST_REVIEWED.md` exists (maintainer Qt review)

Otherwise the status is `LICENSE_DECISION_REQUIRED` and/or
`RELEASE_DISTRIBUTION_BLOCKED`. Archives stay on the workflow run.

Even if those files exist later, the publish job attaches the **CLI zip and
checksums only**. The GUI zip is a private verification artifact until Qt
redistribution is actually reviewed.

Releases are prerelease. Do not mark v0.1.0 as latest/production from this
workflow.

## Do not do from an assistant session

- Choose a license
- Push `v0.1.0` or any other release tag
- Publish a GitHub Release
- Commit Visual C++ runtime DLLs
- Generate or commit a certificate, `.pfx`, or private key
- Weaken `filesystem_mutation: false` so CI is easier
