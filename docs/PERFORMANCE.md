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

### Incremental speedup (when USN is available)

On this development machine, opening `\\.\D:` for USN requires administrator or
backup privilege. Without it, **no USN delta path runs**, so there is **no valid
incremental-vs-full speedup ratio** to publish for 1 / 100 / ~1000 file changes.

When elevated (or with SeBackupPrivilege), expected behavior:

1. Full `index` stores `checkpoint.status=ready` with non-zero `usn_journal_id` / `next_usn`
2. Small mutations under the root
3. `index refresh` applies coalesced USN records and advances the checkpoint
4. Wall-clock for refresh should be much smaller than a full rebuild on large trees

Re-measure under elevation before claiming production incremental gains:

```powershell
# elevated shell
.\build-release\cli\spacelens.exe index <root> --json
# mutate N files under <root>
.\build-release\cli\spacelens.exe index refresh <root> --json
.\build-release\cli\spacelens.exe index <root> --json   # full rebuild comparator
```

## Interpretation

- Full index cost is dominated by live scan + per-file metadata; SQLite publish is small.
- Indexed `query` stays in the tens of milliseconds without touching the analyzed tree.
- Incremental refresh correctly **fails closed** when the volume journal cannot be
  opened: previous index remains queryable; agents are told to full-rebuild.
- Do not compare unelevated `index refresh` (~40 ms no-op reject) to full rebuild
  as a “speedup” — that path did not apply deltas.

## Regression guardrails

- Debug + Release: **74** unit tests, including refresh seams that soft-skip parity
  when USN is unavailable in the environment.
- `filesystem_mutation: false` and no journal-mutation FSCTLs in `UsnJournal.cpp`.
