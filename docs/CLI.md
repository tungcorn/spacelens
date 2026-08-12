# SpaceLens CLI

First-class command-line interface for agents and scripts.

## Build

```powershell
. .\scripts\dev-env.ps1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target spacelens
.\build\cli\spacelens.exe version
```

> **Windows note:** the GUI binary is `build/gui/spacelens-gui.exe`. Do not name
> it `SpaceLens.exe` next to `spacelens.exe` — the filesystem is case-insensitive
> and the two targets would overwrite each other.

## Commands

```text
spacelens scan <path> [--json]
spacelens top  <path> --files|--dirs [--limit N] [--json]
spacelens help
spacelens version
```

## Output streams

| Stream | Content |
|--------|---------|
| stdout | Result only (table or JSON) |
| stderr | Errors, optional progress |

## Exit codes

| Code | Meaning |
|-----:|---------|
| 0 | Success |
| 1 | Internal error |
| 2 | Invalid arguments |
| 3 | Inaccessible root |
| 4 | Scan failure |
| 5 | Cancelled |

## JSON

Pass `--json`. Sizes are raw bytes (`size_bytes`). Field names are stable; see `docs/ARCHITECTURE.md`.
