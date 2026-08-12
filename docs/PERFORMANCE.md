# Performance Notes

Measured for Persistent Index V1 and Incremental Index V2. Numbers are wall-clock
from the Release CLI process, warm OS cache, single run unless noted. Treat them
as a local baseline, not a competitive claim.

## Methodology

| Item | Value |
|------|--------|
| Build | Release, Ninja, `CMAKE_BUILD_TYPE=Release` |
| Toolchain | MSVC 19.50 (VS 2026), Windows SDK 10.0.26100 |
| Qt | 6.8.3 (GUI built; benchmarks use CLI only) |
| Warm-up | Prior build/index of similar trees; OS file cache warm |
| Repetitions | Single timed wall-clock capture (Stopwatch around process) |
| Scope | Project-local trees only — no system drive, no mutation of source trees beyond synthetic fixtures |
| Units | Binary sizes (1024); times in milliseconds wall-clock |

CLI binary:

```text
build-release/cli/spacelens.exe
```

```powershell
. .\scripts\dev-env.ps1
.\build-release\cli\spacelens.exe scan <root> --json
.\build-release\cli\spacelens.exe index <root> --json
.\build-release\cli\spacelens.exe index refresh <root> --json
.\build-release\cli\spacelens.exe query <root> --files --limit 20 --json
```

## Hardware and OS

| Item | Value |
|------|--------|
| Machine | TUNGCORN |
| CPU | AMD64 Family 25 Model 80 (AuthenticAMD) |
| RAM | ~13.8 GiB |
| OS | Microsoft Windows 11 Home Single Language 10.0.26200 |
| Storage | Local NTFS (project on `D:`) |
| Elevation | **Unelevated** interactive user (no SeBackupPrivilege effective) |

## Dataset A — project tree (V1 baseline, 2026-08-12)

| Field | Value |
|-------|--------|
| Root | `D:\Hoc\MyProjects\spacelens` (source + build trees) |
| Files (indexed) | 530 |
| Directories (indexed) | 258 |
| Logical bytes | 138 499 453 (~132 MiB) |
| Index DB size | ~356 KiB |

| Operation | Wall-clock | Notes |
|-----------|------------|--------|
| Live `scan` (Release) | **148 ms** | Process-level timer |
| `index` full rebuild | **142 ms** process / **84 ms** `elapsed_ms` | Scan + SQLite publish |
| `query --files --limit 20` | **27 ms** | `source: persistent_index` |
| `query --dirs --limit 20` | **26 ms** | |
| `query --files --ext cpp --limit 20` | **25 ms** | 53 matches |

## Dataset B — synthetic 2000 files (V2, 2026-08-12)

| Field | Value |
|-------|--------|
| Root | `D:\Hoc\MyProjects\spacelens\_bench_refresh_v2` |
| Files | 2000 |
| Directories | 51 |
| Logical bytes | 101 000 |

| Operation | Wall-clock | Notes |
|-----------|------------|--------|
| `index` full rebuild | **473 ms** process / **404 ms** `elapsed_ms` | Schema v2 + FRN capture + checkpoint attempt |
| `index refresh` (unelevated) | **43 ms** process / **0 ms** engine | Immediate `full_rebuild_required`, reason `access_denied` |
| `index status` | tens of ms | Reports `incremental_refresh.state: access_denied` |

### Incremental speedup — elevated happy path (2026-08-12)

Unelevated interactive sessions still see `access_denied` for volume USN open
(no effective SeBackupPrivilege). That path is **ENVIRONMENT_BLOCKED**, not a
performance result.

**Elevated** verification via `scripts/verify-usn-refresh.ps1` on temp fixtures
only (no analyzed-tree mutation, no journal create/configure):

| Field | Value |
|-------|--------|
| Evidence | `usn-verify-elevated.json` (local run artifact; not required in tree) |
| Outcome | **PASS** (create/modify/delete/rename + subdir boundary parity) |
| Volume open | ok |
| Checkpoint | `ready` after full index |
| Drive | `C:` NTFS |

| Operation | Wall-clock | Notes |
|-----------|------------|--------|
| Full rebuild (5000 files) | **438 ms** process / **379 ms** engine | Synthetic tree under `%TEMP%` |
| Incremental 1 change | **53 ms** process / **29 ms** engine | `outcome: refreshed`, 9 journal records, 1 row changed |
| Query after refresh | **26 ms** | `limit 20` |
| Incremental 100 / 1000 batch | n/a | Script observed `full_rebuild_required` with 0 journal records after the 1-change refresh — **not** claimed as a speedup; investigate later |

Parity: incrementally refreshed index matched independent full rebuild on paths,
sizes, counts, and classification/reclaim fields for the mutation scenarios.

```powershell
# Prefer an already-elevated shell (avoid repeated UAC popups from the harness)
. .\scripts\dev-env.ps1
.\scripts\verify-usn-refresh.ps1 -CliPath .\build-release\cli\spacelens.exe
```

## Interpretation

- Full index cost is dominated by live scan + per-file metadata; SQLite publish is small.
- Indexed `query` stays in the tens of milliseconds without touching the analyzed tree.
- Incremental refresh correctly **fails closed** when the volume journal cannot be
  opened: previous index remains queryable; agents are told to full-rebuild.
- Do not compare unelevated `index refresh` (~40 ms no-op reject) to full rebuild
  as a “speedup” — that path did not apply deltas.
- When elevated and the checkpoint is ready, a **1-change** refresh on a 5000-file
  tree was ~8–13× faster than full rebuild in this run (process/engine). Larger
  batch timings were not validated in the same pass.

## Regression guardrails

- Debug + Release unit tests include refresh seams, USN parity (soft-skip when
  USN is unavailable), and IndexCatalog freshness mapping.
- `filesystem_mutation: false` and no journal-mutation FSCTLs in `UsnJournal.cpp`.
