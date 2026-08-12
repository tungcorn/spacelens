# SpaceLens

SpaceLens is a native Windows disk-space analyzer built with modern C++20, CMake, and Qt 6 Widgets. Its initial goal is to scan a selected directory, aggregate space usage through the directory hierarchy, and present the results in a responsive desktop interface.

## Status

Phase 1 is in progress. The repository currently contains the project planning and architecture documentation; implementation and performance measurements should not be inferred until they are added and validated.

## Planned features

- Scan a selected Windows directory using native filesystem enumeration.
- Show recursive directory sizes in a navigable tree.
- List files and subdirectories for the selected location.
- Show a bounded Top-K list of the largest files.
- Report progress, access limitations, errors, and cancellation clearly.
- Open selected paths in Windows Explorer and copy paths to the clipboard.
- Later phases may add history, richer analysis, and other explicitly planned capabilities.

## Build prerequisites

- Windows 10/11 x64
- MSVC (Visual Studio 2022/2026 C++ tools) with Windows 10/11 SDK
- CMake 3.21+
- Ninja (recommended) or MSBuild
- Qt 6.8+ (Widgets, Concurrent) — e.g. `D:\Qt\6.8.3\msvc2022_64`

See [`docs/SURVEY.md`](docs/SURVEY.md) for the environment used during bootstrap.

## Build and run

```powershell
# From a shell where cl.exe and the Windows SDK are available:
$env:CMAKE_PREFIX_PATH = "D:\Qt\6.8.3\msvc2022_64"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\SpaceLens.exe
```

Debug:

```powershell
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="D:\Qt\6.8.3\msvc2022_64"
cmake --build build-debug
ctest --test-dir build-debug --output-on-failure
```

If `cl` is not on PATH, run the Visual Studio developer environment first, or use `scripts\dev-env.ps1` once it is added.

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
