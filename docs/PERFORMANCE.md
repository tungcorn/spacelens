# Performance Notes

Measured on the Persistent Index V1 milestone. Numbers are wall-clock from the
Release CLI process, warm OS cache, single run unless noted. Treat them as a
local baseline, not a competitive claim.

## Methodology

| Item | Value |
|------|--------|
| Build | Release, Ninja, `CMAKE_BUILD_TYPE=Release` |
| Toolchain | MSVC 19.50 (VS 2026), Windows SDK 10.0.26100 |
| Qt | 6.8.3 (GUI built; benchmarks use CLI only) |
| Commit series | Persistent Index V1 on `main` after GUI milestone `99edcf0` |
| Warm-up | One prior Debug build/index of the same tree; OS file cache warm |
| Repetitions | Single timed wall-clock capture (Stopwatch around process) |
| Scope | Project tree only — no system drive, no mutation |
| Units | Binary sizes (1024); times in milliseconds wall-clock |

CLI binary:

```text
build-release/cli/spacelens.exe
```

Commands:

```powershell
. .\scripts\dev-env.ps1
# live scan
.\build-release\cli\spacelens.exe scan <root> --json
# index build (includes live scan + SQLite publish)
.\build-release\cli\spacelens.exe index <root> --json
# indexed queries (no live scan)
.\build-release\cli\spacelens.exe query <root> --files --limit 20 --json
.\build-release\cli\spacelens.exe query <root> --dirs --limit 20 --json
.\build-release\cli\spacelens.exe query <root> --files --ext cpp --limit 20 --json
```

## Hardware and OS

| Item | Value |
|------|--------|
| Machine | TUNGCORN |
| CPU | AMD64 Family 25 Model 80 (AuthenticAMD) |
| RAM | ~13.8 GiB |
| OS | Microsoft Windows 11 Home Single Language 10.0.26200 |
| Storage | Local NTFS (project on `D:`) |

## Dataset

| Field | Value |
|-------|--------|
| Root | `D:\Hoc\MyProjects\spacelens` (source + build trees) |
| Files (indexed) | 530 |
| Directories (indexed) | 258 |
| Logical bytes | 138 499 453 (~132 MiB) |
| Index DB size | 364 544 bytes (~356 KiB) |
| Index path | `%LOCALAPPDATA%\SpaceLens\indexes\<rootKey>\index.db` |
| Notes | Includes `build/`, `build-release/`, `third_party/sqlite/sqlite3.c` |

## Measured results (2026-08-12)

| Operation | Wall-clock | Notes |
|-----------|------------|--------|
| Live `scan` (Release) | **148 ms** | Process-level timer; JSON discarded |
| `index` full rebuild | **142 ms** process / **84 ms** reported `elapsed_ms` | Scan + classify + SQLite staging + publish |
| `query --files --limit 20` | **27 ms** | `source: persistent_index` |
| `query --dirs --limit 20` | **26 ms** | Directory recursive sizes |
| `query --files --ext cpp --limit 20` | **25 ms** | 53 matched `.cpp` files |
| `query --files --min-size 1MB --limit 5` | ~25–30 ms | 25 matched ≥ 1 MiB |
| `query --files --min-size 10MB` | ~26 ms | 0 matches on this tree (largest file ~9.9 MiB) |

### Interpretation

- On a ~530-file developer tree, full index build is on the same order as a single
  live scan (scan dominates; SQLite write is small).
- Subsequent filtered queries complete in tens of milliseconds without touching
  the analyzed tree.
- DB footprint is small relative to scanned logical bytes (~356 KiB DB vs ~132 MiB
  logical content for this dataset).
- Larger roots (user profiles, drive roots) will shift the ratio: expect scan and
  insert cost to grow roughly with entry count; query latency should remain low
  while indexes and `LIMIT` are used.

## Metrics still useful for later milestones

- Cold-cache full-drive scans
- Peak working set during index build
- Cancellation response during large rebuilds
- Incremental / USN refresh cost (not implemented in V1)
- Live `find` vs indexed `query` parity timing on multi-million entry trees

## Correctness checks tied to performance runs

- `capabilities --json` reports `persistent_index: true`, `indexed_query: true`,
  `incremental_index: false`, `filesystem_mutation: false`
- Missing index query returns exit code **6**
- Cancelled rebuild leaves previous `index.db` intact
- Unit suite: **65+** tests green (Debug and Release)

## Safety note

All benchmarks are **read-only** against the analyzed root. Index files are
written only under `%LOCALAPPDATA%\SpaceLens\`. No source files are deleted or
moved by `index` or `query`.
