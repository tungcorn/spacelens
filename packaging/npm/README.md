# @tungcorn/spacelens

SpaceLens is native C++ storage intelligence for Windows.

This npm package is an installation channel. It contains the current
npm distribution: the desktop GUI, the read-only CLI, and the
read-only MCP server, plus the Qt 6.8.3 runtime the GUI needs. Node
and npm are required only to install and launch this package.
SpaceLens itself remains native C++.

This is not a Node SDK. The wrappers do not add commands, transform
JSON, or authorize filesystem maintenance.

## Requirements

- Windows x64 only (`os: win32`, `cpu: x64`)
- Node.js 18+ (for the launchers)
- [Microsoft Visual C++ Redistributable (x64)](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)

Binaries are unsigned.

## Install

```text
npm install -g @tungcorn/spacelens
```

The tarball already includes the native runtime. Installation does not
download further GitHub or npm assets.

## Commands

```text
spacelens
spacelens-mcp
spacelens-gui
```

- `spacelens` — command-line storage intelligence
- `spacelens-mcp` — read-only stdio MCP server for AI agents
- `spacelens-gui` — native desktop interface

CLI examples:

```text
spacelens version
spacelens help
spacelens capabilities --json
spacelens overview C:\path\to\folder --json
spacelens opportunities C:\path\to\folder --json
```

`spacelens ... --json` is safe for scripts and agents: the launcher
prints no banner and does not rewrite native stdout.

`spacelens-mcp` is stdio MCP. Pair it with an MCP client. The
launcher prints no banner; stdout is protocol only.

## Safety

The CLI and MCP adapter are read-only.

```text
spacelens capabilities --json
```

reports `filesystem_mutation: false`.

They cannot recycle, restore, delete, move, or declare locations.
Filesystem maintenance remains GUI-only and explicitly human-authorized
(Recycle Bin, after confirmation). The npm wrapper exposes no mutation
API. AI recommendation is not filesystem permission.

Uninstalling the npm package removes the staged binaries and shims. It
does not delete `%LOCALAPPDATA%\SpaceLens\` (indexes, review state,
maintenance history, hash cache).

## License

SpaceLens-owned files are MIT. See `LICENSE`.

Qt remains under its own licenses (LGPL-3.0-only option for this
dynamic build). Qt is not MIT. Corresponding-source identity and the
written offer are in `licenses/`.
