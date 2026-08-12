# Environment Survey (2026-08-12)

Read-only survey of the development machine before implementation.

## Repository

| Item | Result |
|------|--------|
| Path | `D:\Hoc\MyProjects\spacelens` |
| Git | Initialized, branch `main`, no commits yet |
| Remote | `origin` → `https://github.com/tungcorn/spacelens.git` |
| Source tree | Empty except `.git` at survey time |

## Toolchain

| Tool | Status | Notes |
|------|--------|-------|
| CMake | Available | 4.3.1 (`C:\Program Files\CMake\bin`) |
| Ninja | Available | 1.11.0 |
| Git | Available | 2.47.0.windows.1 |
| MSVC (`cl.exe`) | Present | VS Community 2026 at `D:\visual-studio-2026`, MSVC 14.50.35717 |
| `vswhere` instances | Empty | VS not fully registered; `vcvars64.bat` needs manual path setup |
| Windows SDK headers | Missing at survey | `Windows.h` not found; SDK 10.0.26100 install started afterward |
| Qt 6 | Missing at survey | Installed later via `aqtinstall` to `D:\Qt\6.8.3\msvc2022_64` |
| MinGW g++ | Present | `D:\Downloads\mingw64` (fallback only; primary is MSVC) |
| Electron / Python core | Not used | Per product rules, scanner stays C++ |

## Disk

| Drive | Free (approx.) |
|-------|----------------|
| C: | ~36 GB |
| D: | ~66 GB |

Preferred install location for large SDKs/Qt: `D:\`.

## Implications for Phase 1

1. Build scripts must accept `CMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64`.
2. MSVC environment may need a project helper (batch/PowerShell) that sets `INCLUDE`/`LIB`/`PATH` when `vcvars` fails due to incomplete VS registration.
3. Do not claim a green Release build until Windows SDK desktop headers/libs are present and a compile test succeeds.
4. Core library should remain Qt-free so unit tests can run once the C++ toolchain works, independent of UI polish.
