# Contributing to SpaceLens

## Product boundary

```text
External AI / script  →  spacelens CLI  →  read-only filesystem intelligence
Human                 →  SpaceLens GUI  →  review → explicit Recycle Bin
```

Do not add CLI verbs that delete, recycle, restore, move, or write safety
declarations. Do not link `spacelens_maintenance` into `spacelens`.

## Prerequisites

- Windows 10/11 x64
- CMake 3.21+
- Ninja
- MSVC with the Windows SDK
- Qt 6.8.3 Widgets + Concurrent, only if you build the GUI

Set `CMAKE_PREFIX_PATH` to the Qt prefix, or run `. .\scripts\dev-env.ps1`.
Do not commit machine-specific Qt or Visual Studio paths.
`CMakeUserPresets.json` is gitignored for local overrides.

## Build

```powershell
. .\scripts\dev-env.ps1
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
.\build-debug\tests\spacelens_tests.exe
```

Release (GUI + CLI):

```powershell
.\scripts\configure-release.ps1
.\scripts\build-release.ps1
```

CLI only, no Qt:

```powershell
cmake --preset windows-cli-release
cmake --build --preset windows-cli-release
ctest --preset windows-cli-release
```

After a Release build:

```powershell
.\scripts\verify-cli-safety.ps1 -CliPath .\build-release\cli\spacelens.exe
```

## Tests

- Generated temporary fixtures only.
- Do not point tests or stress at user data or the project source tree.
- Do not commit personal absolute paths.

## Pull requests

- Keep changes coherent. Do not mix product features with release-engineering
  or formatting-only noise.
- CI must stay green: Full Debug, Full Release, CLI-only, and `/analyze`.
- The Release job runs the CLI safety script. Do not skip or weaken it.
- Pin any new GitHub Action to a verified full-length commit SHA. Do not invent
  SHAs.

## Security contact

This repository does not yet publish a security contact or `SECURITY.md`.
Do not invent one. If you find a vulnerability that would put users at risk in
a public issue, wait until a maintainer designates a contact.

## License

No project license has been chosen. See `docs/LICENSE_DECISION_REQUIRED.md`.
Do not add a `LICENSE` file unless a maintainer explicitly selects one.
Do not set `review_status=PASS` or create `docs/QT_REDIST_REVIEWED.md`
unless corresponding Qt source is actually under maintainer control.
