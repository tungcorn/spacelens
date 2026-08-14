# Contributing to SpaceLens

## Product boundary

```text
External AI / script  →  spacelens CLI      →  read-only filesystem intelligence
External AI harness   →  spacelens-mcp      →  same core, typed MCP tools
Human                 →  SpaceLens GUI      →  review → explicit Recycle Bin
```

Do not add CLI verbs or MCP tools that delete, recycle, restore, move, or
write safety declarations. Do not link `spacelens_maintenance` into
`spacelens` or `spacelens-mcp`. AI recommendation is not filesystem
permission.

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
.\scripts\dev.ps1
```

`scripts/dev.ps1` configures if needed, builds the local GUI, and launches
`build-*/gui/spacelens-gui.exe`. It does not publish or download a release.

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
.\scripts\verify-agent-interface.ps1 -CliPath .\build-release\cli\spacelens.exe
.\scripts\verify-indexed-intelligence.ps1 -CliPath .\build-release\cli\spacelens.exe
.\scripts\verify-mcp-safety.ps1 -McpPath .\build-release\mcp\spacelens-mcp.exe
.\scripts\verify-mcp-wire.ps1   -McpPath .\build-release\mcp\spacelens-mcp.exe
.\scripts\verify-mcp-parity.ps1 -CliPath .\build-release\cli\spacelens.exe `
                               -McpPath .\build-release\mcp\spacelens-mcp.exe
```

npm packaging templates (no native rebuild required):

```powershell
.\scripts\verify-npm-template.ps1
```

## Tests

- Generated temporary fixtures only.
- Do not point tests or stress at user data or the project source tree.
- Do not commit personal absolute paths.

## Pull requests

- Keep changes coherent. Do not mix product features with release-engineering
  or formatting-only noise.
- CI must stay green: Full Debug, Full Release, CLI-only, `/analyze`,
  and npm package staging.
- The Release and CLI-only jobs run CLI safety and MCP safety/wire/parity
  scripts. Do not skip or weaken them.
- Pin any new GitHub Action to a verified full-length commit SHA. Do not invent
  SHAs.

## Security contact

This repository does not yet publish a security contact or `SECURITY.md`.
Do not invent one. If you find a vulnerability that would put users at risk in
a public issue, wait until a maintainer designates a contact.

## License

SpaceLens-owned code is MIT. See `LICENSE`. Do not relicense the project,
Qt, or SQLite. An assistant must not change the maintainer-selected MIT
text except to fix a verified transcription error.
Do not set `review_status=PASS` or create `docs/QT_REDIST_REVIEWED.md`
unless corresponding Qt source is actually under maintainer control.
