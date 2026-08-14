# @tungcorn/spacelens

SpaceLens is native C++ storage intelligence for Windows.

This npm package is an installation channel. It contains the complete
v0.1.1 distribution: the desktop GUI and the read-only CLI, plus the Qt
6.8.3 runtime those binaries need. Node and npm are required only to
install and launch this package. SpaceLens itself remains native C++.

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
spacelens-gui
```

CLI examples (real verbs from SpaceLens v0.1.1):

```text
spacelens version
spacelens help
spacelens capabilities --json
spacelens scan C:\path\to\folder --json
spacelens top C:\path\to\folder --json
```

`spacelens ... --json` is safe for scripts and agents: the launcher
prints no banner and does not rewrite native stdout.

## Safety

The CLI is read-only.

```text
spacelens capabilities --json
```

reports `filesystem_mutation: false`.

The CLI cannot recycle, restore, delete, move, or declare locations.
Filesystem maintenance remains GUI-only and explicitly human-authorized
(Recycle Bin, after confirmation). The npm wrapper exposes no mutation
API.

Uninstalling the npm package removes the staged binaries and shims. It
does not delete `%LOCALAPPDATA%\SpaceLens\` (indexes, review state,
maintenance history).

## License

SpaceLens-owned files are MIT. See `LICENSE`.

Qt remains under its own licenses (LGPL-3.0-only option for this
dynamic build). Qt is not MIT. Corresponding-source identity and the
written offer are in `licenses/`.
