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

### Incremental Index V2.1 — elevated multi-batch evidence (2026-08-12)

Unelevated sessions still see `access_denied` for volume USN open. That path is
**ENVIRONMENT_BLOCKED**, not a performance result.

**Elevated** verification via `scripts/verify-usn-refresh.ps1` on `%TEMP%`
fixtures only (no analyzed-tree mutation, no journal create/configure).

| Field | Value |
|-------|--------|
| Evidence | `usn-verify-elevated.json` (local; gitignored pattern `usn-verify-*.json`) |
| Outcome | **PASS** |
| Elevation | Administrator |
| Volume open | ok (`\\.\C:`) |
| Filesystem | `C:` NTFS Healthy |
| CPU | AMD Ryzen 5 7530U (6C/12T) |
| RAM | ~14 GiB |
| OS | Windows 11 Home 10.0.26200 x64 |
| CLI | Release `build-release/cli/spacelens.exe` |
| Methodology | Single full script run (~21 s wall); process Stopwatch + CLI `elapsed_ms` |
| Cursor | Driver READ continuation; batch N+1 `start_usn` == batch N `committed_next_usn` |

#### Correctness matrix (parity = incremental query snapshot vs fresh full rebuild)

| Scenario | Method | Parity | Refresh (process / engine) | Full rebuild (process) | Notes |
|----------|--------|--------|----------------------------|------------------------|-------|
| create/modify/delete/rename | `refreshed` | **PASS** | 31 ms / 13 ms | 83 ms | 33 journal records; counts/bytes match |
| subdir root-boundary in/out | `refreshed` | **PASS** | 29 ms / 12 ms | 82 ms | Outside paths excluded |
| multi-batch 1 change | `refreshed` | (see batch parity) | 31 ms | — | `added=1`; cont advances |
| multi-batch 100 changes | `refreshed` | (see batch parity) | 62 ms | — | `start` = prior `committed` |
| multi-batch 1000 changes | `refreshed` | **PASS** (after 1+100+1000) | 340 ms | — | same index; files=1307 dirs=108 bytes=26006 match full |
| process restart | `refreshed` ×2 | **PASS** | 13 ms then 11 ms engine | — | post.`start_usn` == pre.`committed_next_usn` |

No scenario counted a `full_rebuild_required` as incremental success.

#### Large-tree sequential bench (5000-file fixture, same index, all `outcome: refreshed`)

| Operation | Process | Engine | Records seen | Rows changed |
|-----------|---------|--------|--------------|--------------|
| Full rebuild | **595 ms** | **538 ms** | — | — |
| Incremental 1 change | **59 ms** | **34 ms** | 21 | 1 |
| Incremental 100 changes | **108 ms** | **86 ms** | 333 | 100 |
| Incremental 1000 changes | **345 ms** | **328 ms** | 3126 | 1000 |
| Query after refresh (`--files --limit 20`) | **26 ms** | — | — | — |

Approximate speedup vs full rebuild on this run (process wall-clock): ~10× (1 change),
~5.5× (100), ~1.7× (1000). Larger batches approach full-rebuild cost as expected;
do not treat as a universal claim beyond this machine/fixture.

```powershell
# Already-elevated PowerShell (avoid -SelfElevate UAC spam from the harness)
. .\scripts\dev-env.ps1
.\scripts\verify-usn-refresh.ps1 -CliPath .\build-release\cli\spacelens.exe -LargeFileCount 5000 -ReportPath .\usn-verify-elevated.json
```

Fixes closed during this gate (beyond the continuation-cursor fix):

1. Directory aggregate recompute order was root→leaf (parents kept pre-delta sizes).
2. Same-window new parent directory + children could hit `missing_parent` because FRN apply order is unordered — parents are now ensured from live disk under the root.

## Dataset C — generated 100k+ fixture (Release Engineering V0.1, 2026-08-13)

Generated by `scripts/stress-v01.ps1`. Temporary fixture only — not the
project source tree and not user data. Path in the report is redacted to
`<temp>/spacelens-stress-*/fixture`.

| Item | Value |
|------|--------|
| Build | Release CLI `build-release/cli/spacelens.exe` |
| Entries | **100041** (99944 files + 97 directories) |
| Logical bytes | 59 633 024 (~56.9 MiB) |
| Shape | 20 wide buckets, 40 nested levels, 32-deep `d\d\…`, Unicode `unicode-名称` |
| Index DB size | 52 736 000 bytes (~50.3 MiB) |
| Cache | Fixture generation + first scan likely warmed the OS cache |
| Query repeats | 11; p50 / p95 from the sorted sample list |

| Operation | Wall-clock | Notes |
|-----------|------------|--------|
| Live `scan --json` | **330 ms** process | First timed scan after generation |
| `index` full rebuild | **15676 ms** process / **15395 ms** `elapsed_ms` | Schema v2 + SQLite publish |
| `query --files --limit 20` | p50 **41 ms**, p95 **49 ms** (min 37, max 54) | Warm |
| `query --dirs --limit 20` | p50 **40 ms**, p95 **46 ms** (min 33, max 77) | Warm |
| `query --files --ext dat --limit 20` | p50 **39 ms**, p95 **52 ms** (min 36, max 60) | Warm |
| Cancel | best-effort `Stop-Job` after 80 ms | Job state `Stopped`; not a proof of cooperative cancel |

Hardware matches Dataset A (same machine / OS / unelevated). This is a
single-run generated-tree baseline, not a competitive claim.

```powershell
.\scripts\stress-v01.ps1 -Entries 100000 -CliPath .\build-release\cli\spacelens.exe
```

Not run on every pull request. CI CLI-only uses a 2k smoke. Manual 100k+
is `.github/workflows/quality.yml`.

## Storage Intelligence / Agent Interface V1 (2026-08-14)

Temporary developer-workstation fixture from
`scripts/verify-agent-interface.ps1` (~96 MiB logical: `node_modules`,
CMake `build`, `.cache`, old zip, recent 40 MiB VM image, photo,
independent 2 MiB copies, one hard-link pair). Not the source tree and
not user data. Same machine family as Dataset A. OS cache likely warm
after fixture generation.

| Build | Operation | Wall-clock | JSON | Notes |
|-------|-----------|------------|------|--------|
| Release `build-release/cli/spacelens.exe` | `overview --json` | **21 ms** | 8283 B | One live scan; 10+10 consumers |
| Release | `scan` + `top --dirs` + `top --files` | **57 ms** | — | Three separate process launches |
| Release | `opportunities --json` | (script step) | 6439 B | 8 items; `unique_review_bytes=55578528` |
| Debug `build-debug/cli/spacelens.exe` | `overview --json` | **27–28 ms** | 8283 B | Same fixture |
| Debug | `scan` + `top --dirs` + `top --files` | **65–82 ms** | — | Three launches |

`overview` is one scan. `scan` + `top --dirs` + `top --files` rescans
twice more. The JSON payloads stay in the low kilobytes so an agent can
read them without enumerating the tree. These are single-run process
wall-clock numbers, not a competitive claim.

```powershell
.\scripts\verify-agent-interface.ps1 -CliPath .\build-release\cli\spacelens.exe
```

## Indexed Intelligence Scaling V1 (2026-08-14)

Exact top-N for indexed `opportunities` (`opportunity_rank_v2` in SQLite,
filters before `LIMIT`). Isolated temp fixtures / `SPACELENS_DATA_ROOT`
only — not the source tree and not user AppData. Same machine family as
Dataset A. Single-run wall-clock, not a competitive claim. Schema stays
`index_schema_version: 2`; no new SQLite indexes.

| Build | Fixture | Operation | Wall-clock | Notes |
|-------|---------|-----------|------------|--------|
| Release `build-release/cli/spacelens.exe` | 250 cmake-build decoys + late `node_modules` (503 files) | `index` | **349 ms** | Real CLI publish |
| Release | same | `opportunities --from-index --limit 5` | **55 ms** / 5105 B JSON | First row is High `node_modules`, not a larger Medium decoy |
| Release `spacelens_tests.exe` | 10 003 synthetic SQL rows | oracle vs production top-20 | **984 ms** process | Includes isolated publish |
| Release | 100 001 synthetic SQL rows + tail `node_modules` | hidden-candidate + JSON bound | **2967 ms** process | JSON under 200 KB; top-N exact; unique estimated only if matches exceed 50k |

```powershell
.\scripts\verify-indexed-intelligence.ps1 -CliPath .\build-release\cli\spacelens.exe
$env:SPACELENS_TEST_ONLY = "IndexedIntel_100k"
.\build-release\tests\spacelens_tests.exe
```

250k+ physical trees were not required: the 100k gate is synthetic SQL
rows under an isolated data root. Incremental USN timings are unchanged
(no schema/index change).

## Interpretation

- Full index cost is dominated by live scan + per-file metadata; SQLite publish is small.
- Indexed `query` stays in the tens of milliseconds without touching the analyzed tree.
- Incremental refresh correctly **fails closed** when the volume journal cannot be
  opened: previous index remains queryable; agents are told to full-rebuild.
- Do not compare unelevated `index refresh` (~40 ms no-op reject) to full rebuild
  as a “speedup” — that path did not apply deltas.
- Elevated multi-batch + restart parity is the V2.1 reliability bar; small-delta
  refreshes are substantially cheaper than full rebuild on large trees.

## Regression guardrails

- Debug + Release unit tests include refresh seams, multi-refresh / reopen USN
  parity (soft-skip when USN is unavailable), and IndexCatalog freshness mapping.
- `scripts/verify-usn-refresh.ps1` fails overall if multi-batch or restart refresh
  outcomes are not `refreshed`/`already_current`.
- `filesystem_mutation: false` and no journal-mutation FSCTLs in `UsnJournal.cpp`.
