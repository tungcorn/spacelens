# SpaceLens CLI

The SpaceLens CLI is a first-class, agent-safe interface for humans, scripts,
and AI agents. It performs read-only scanning and storage analysis. It does not
delete, move, rename, purge, wipe, execute shell commands, or grant filesystem
mutation authority.

The executable wires `scan`, `top`, `find`, `index`, `index refresh`,
`index status`, `index list`, `query`, `duplicates`, `capabilities`, `help`,
and `version`. Use `capabilities --json` to discover flags for a particular
build (including `persistent_index`, `indexed_query`, and `incremental_index`).

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

## Read-only contract

The CLI is safe to grant to an agent for observation and query. It has no
filesystem mutation commands or hidden destructive aliases. A future mutation
surface, if ever approved, must be separately permissioned and must not be added
as an ordinary verb to this executable.

The CLI does not interpret AI-generated text as commands. Paths are data and are
passed to structured C++/Win32 APIs, not shell command strings.

## Commands

### `capabilities`

Reports the supported command and filter surface. JSON is recommended for agents.

```text
spacelens capabilities [--json]
```

The intended JSON response includes:

Representative JSON (fields may grow; treat unknown keys as forward-compatible):

```json
{
  "schema_version": 1,
  "version": "0.1.0",
  "commands": ["scan", "top", "find", "index", "index status", "index list", "index refresh", "query", "duplicates", "capabilities", "help", "version"],
  "features": {
    "json": true,
    "cancellation": true,
    "persistent_index": true,
    "indexed_query": true,
    "incremental_index": true,
    "filesystem_mutation": false,
    "classification": true,
    "filters": true
  },
  "read_only": true,
  "filesystem_mutation": false,
  "index_schema_version": 2
}
```

A build must not be assumed to support a command or filter until it advertises
that capability. Indexed query requires a prior successful `index` for that root.

### `scan`

Scans a directory and reports counts, logical bytes, errors, and elapsed time.

```text
spacelens scan <path> [--json]
```

`scan` does not modify the scanned path. By default directory reparse points are
not followed; skipped points are counted in the result.

### `top`

Returns the largest files or directories found during a scan.

```text
spacelens top <path> --files [--limit N] [--json]
spacelens top <path> --dirs  [--limit N] [--json]
```

`--limit N` bounds the returned result set and defaults to 20 in the CLI argument
contract. Top-K collection is bounded; it is not a mutation or cleanup operation.

### `find`

Runs a read-only storage-intelligence query over a scan snapshot. It is intended
to return files and/or directories matching the requested filters, together with
classification, location-safety, activity, reclaimability, candidate-strength,
and explanation fields where available.

```text
spacelens find <path> [--min-size SIZE] [--older-than DAYS]
                    [--category CATEGORY] [--files|--dirs]
                    [--limit N] [--json]
```

`find` is analysis, not deletion. It does not add results to Cleanup Review
unless a separate human UI action explicitly chooses to do so. It never changes
the filesystem. Unlike `query`, `find` always performs a live scan.

### `index`

Builds or rebuilds the persistent SQLite index for a root (full scan + publish).

```text
spacelens index <path> [--json]
```

The index is written under `%LOCALAPPDATA%\SpaceLens\indexes\<rootKey>\`.
Rebuilds stage to `index.db.building` and publish atomically; cancel/fail leaves
any previous good index intact. See [`docs/INDEX.md`](INDEX.md).

### `index status` / `index list`

```text
spacelens index status <path> [--json]
spacelens index list [--json]
```

`status` reports existence, age, counts, readiness, and an
`incremental_refresh` block (supported/state/reason/checkpoint). `list`
enumerates published indexes under AppData. Neither command mutates user data.

### `index refresh`

```text
spacelens index refresh <path> [--json]
```

Applies a **read-only USN** incremental update when a ready checkpoint exists and
the volume journal can be opened. Never creates or configures the journal. On
`access_denied`, `journal_not_active`, journal discontinuity, or missing parent,
returns `full_rebuild_required` and leaves the previous index intact. Does **not**
run automatically from `query`.

### `query`

Runs a filtered query against a **published** index only. No live-scan fallback.

```text
spacelens query <path> [--files|--dirs] [--min-size SIZE] [--ext EXT]
                     [--older-than DAYS] [--category CATEGORY]
                     [--strength STRENGTH] [--limit N] [--json]
```

Missing index → exit code **6** (`index_not_found`). Successful JSON includes
`"source": "persistent_index"` and `index.age_ms`.

### `duplicates`

Finds exact file-content copies from a published index, then live-verifies them.
Same-size files are candidates only. Sample fingerprints are not proof.

```text
spacelens duplicates <indexed-root> [--min-size S] [--json]
```

Default `--min-size` is `1MB`. Zero-length files, directories, and reparse
points are ignored. Hard-link aliases of one identity are reported as the same
file (redundant logical bytes = 0). JSON includes `planning_only`, `read_only`,
and `filesystem_mutation: false`. Missing index → exit **6**. Cancel → exit
**5** with completed groups marked partial.

`--delete`, `--dedupe`, and `--keep-one` are unknown options. See
[`docs/DUPLICATES.md`](DUPLICATES.md).

### `help` and `version`

```text
spacelens help
spacelens version
```

`help` prints usage. `version` prints the product version. These commands do not
scan or mutate the filesystem.

## Filters

| Filter | Meaning |
|--------|---------|
| `--min-size SIZE` | Include items whose logical size is at least `SIZE`. |
| `--older-than DAYS` | Require write-based activity at least this many days old. For directories, use newest descendant write activity. |
| `--category CATEGORY` | Restrict deterministic storage classification, such as `build-artifact`, `package-cache`, `ide-cache`, `temporary-data`, or `user-data`. |
| `--files` | Include files; with `top`, choose largest files. |
| `--dirs` | Include directories; with `top`, choose largest directories. |
| `--limit N` | Return no more than `N` results. |
| `--json` | Write machine-readable JSON to stdout. |

Filter behavior is conservative. Filters do not override Protected or Sensitive
location policy, do not convert old data into deletion permission, and do not
produce a `safe_to_delete` field.

### Size units

Size input uses binary powers of 1024, not decimal SI powers of 1000.

```text
1 KB = 1 KiB = 1024 B
1 MB = 1024 KB
1 GB = 1024 MB
1 TB = 1024 GB
```

Accepted units are `B`, `K`/`KB`/`KiB`, `M`/`MB`/`MiB`, `G`/`GB`/`GiB`, and
`T`/`TB`/`TiB`. Values may contain a decimal fraction, for example `1.5 GB`.
A bare number is bytes. Decimal SI units are intentionally not accepted.

JSON size fields are integer logical bytes, named `size_bytes`. Human output may
use `KB`, `MB`, `GB`, and `TB` labels using the same 1024-based conversion.

## Output streams

| Stream | Content |
|--------|---------|
| stdout | The primary result only: human table/summary or JSON |
| stderr | Diagnostics, optional progress, cancellation, and errors |

When `--json` is used, decorative text must not be written to JSON stdout. Agents
should parse stdout and treat stderr as diagnostics.

## JSON contract

JSON responses use a stable integer `schema_version`. The milestone contract
starts at version `1`; incompatible field or semantic changes require a new
version. Additive fields must not change the meaning of existing fields.

Scan and top responses retain these common fields where applicable:

```json
{
  "schema_version": 1,
  "ok": true,
  "command": "top",
  "root": "C:\\Users",
  "files_scanned": 182391,
  "directories_scanned": 18432,
  "bytes_scanned": 1234567890,
  "elapsed_ms": 8241,
  "access_denied": 4,
  "reparse_skipped": 3,
  "other_errors": 0,
  "state": "completed",
  "results": [
    { "path": "C:\\Users\\example\\build", "size_bytes": 0 }
  ]
}
```

- `size_bytes` is always an integer count of logical bytes.
- `bytes_scanned` is also an integer byte count.
- `state` may be `completed`, `cancelled`, `failed`, or another documented
  lifecycle state where applicable.
- `find` results may add `classification`, `confidence`, `location_safety`,
  `activity`, `reclaimability`, `candidate_strength`, and `explanation`.
- No response may expose `safe_to_delete`; review priority is not authorization.

If a build has not yet added `schema_version` to its executable JSON envelope,
treat that output as pre-contract and do not infer compatibility from field names
alone.

## Safety-related analysis semantics

Classification says what an item appears to be. Location policy says how cautious
the product must be about where it is. Reclaimability says whether that kind of
data is typically regenerable. Activity is primarily write/descendant-write
evidence. These are separate fields and must not be collapsed into a deletion
boolean.

LastAccessTime is advisory only. It may be included when available, but access
time can be disabled, delayed, or changed by scanners and backup tools. It must
not alone produce a Strong reclaim recommendation.

A scan is a snapshot at time T1; the filesystem may differ at T2. The CLI is
read-only now. Any future mutation service must revalidate existence, item kind,
reparse status, protected-location policy, size/write-time changes, and expected
parent before acting.

## Exit codes

| Code | Meaning |
|-----:|---------|
| 0 | Success, including a completed query with no matches |
| 1 | Unexpected internal error |
| 2 | Invalid arguments, unknown command, or unsupported option/filter |
| 3 | Inaccessible or missing root path |
| 4 | Scan or query failure |
| 5 | Cancelled by Ctrl+C / stop request |
| 6 | Published index not found (`query`, `duplicates`) |

Agents should branch on the exit code before trusting a result as complete. A
successful empty `results` array means no matches, not an error.

## Examples

```powershell
# Human summary
.\build\cli\spacelens.exe scan C:\Users

# Machine-readable scan
.\build\cli\spacelens.exe scan C:\Users --json

# Largest directories, capped at 20
.\build\cli\spacelens.exe top C:\Users --dirs --limit 20 --json

# Intended read-only reclaim query
.\build\cli\spacelens.exe find C:\Users --min-size 500MB --older-than 180 --files --json
```
