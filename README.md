# SpaceLens

Native Windows disk-space analyzer written in modern C++20.

SpaceLens is **core-first**: a UI-independent scanner library with a first-class
**CLI** for humans and AI agents, plus an optional **Qt GUI**.

```text
spacelens_core  →  spacelens (CLI)
                →  SpaceLens (GUI)
```

## Status

- Core scanner: working (Win32 enumeration, aggregation, Top-K, cancellation)
- CLI: in progress (`scan`, `top`, `--json`)
- GUI: basic async scan UI present
- Not yet: persistent index, duplicates, MFT fast path, product AI

## Features

- Fast native recursive scan (logical file sizes)
- Largest files (bounded Top-K) and largest directories
- CLI with human and machine-readable (`--json`) output for agents/scripts
- Optional Qt desktop UI (live progress, cancel, Explorer, clipboard)
- Reparse points not followed by default; access errors non-fatal

## Build prerequisites

- Windows 10/11 x64
- MSVC (Visual Studio 2022/2026 C++ tools) with Windows 10/11 SDK
- CMake 3.21+
- Ninja (recommended) or MSBuild
- Qt 6.8+ (Widgets, Concurrent) — e.g. `D:\Qt\6.8.3\msvc2022_64`

See [`docs/SURVEY.md`](docs/SURVEY.md) for the environment used during bootstrap.

## Build and run

```powershell
. .\scripts\dev-env.ps1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CLI only (no Qt required):

```powershell
cmake -S . -B build-cli -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPACELENS_BUILD_GUI=OFF
cmake --build build-cli --target spacelens
.\build-cli\cli\spacelens.exe scan C:\Users --json
.\build-cli\cli\spacelens.exe top C:\Users --dirs --limit 20 --json
```

GUI (Windows output name is `spacelens-gui.exe` — not `spacelens.exe`, because
NTFS is case-insensitive and those names would collide):

```powershell
.\build\gui\spacelens-gui.exe
```

Tests:

```powershell
ctest --test-dir build --output-on-failure
```

See [`docs/CLI.md`](docs/CLI.md) and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Architecture

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the Phase 1–3 layers, ownership model, concurrency policy, and deferred scope. The ordered Phase 1 implementation checklist is in [`docs/PHASE1_PLAN.md`](docs/PHASE1_PLAN.md).

## Roadmap

- **Phase 1:** Functional scanner MVP, indexed data model, aggregation, cancellation/progress, Qt tree UI, largest files, Explorer and clipboard actions.
- **Phase 2:** Usability and analysis improvements around the scanned result, error visibility, navigation, and measured performance work.
- **Phase 3:** A more complete scanning and presentation foundation with hardened platform behavior and maintainable extension points.
- **Phase 4:** Persistent scan history and related storage-backed features, subject to a separately defined design.
- **Phase 5:** Advanced filesystem analysis such as duplicate detection and specialized indexing, subject to explicit scope and validation.
- **Phase 6:** Optional intelligent assistance and other future capabilities, evaluated after the core product is reliable.

No performance claim is made until reproducible measurements are recorded in [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).
