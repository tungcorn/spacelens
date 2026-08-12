# Persistent Index V1

SpaceLens can build a **read-only SQLite index** of a scanned root and answer
repeated size/classification queries without rescanning the filesystem.

This is **not** a mutation surface, incremental USN/MFT watcher, MCP server, or
AI feature. Indexed queries never delete, move, or rewrite source files.

## Goals (V1)

| Goal | Behavior |
|------|----------|
| Full rebuild | `index <root>` scans once and publishes `index.db` |
| Fast re-query | `query` reads only the published DB |
| Safe rebuild | Staging DB + atomic publish; cancel/fail discards staging only |
| Explicit failure | Missing index → exit code **6** (`index_not_found`); **no live fallback** |
| Agent-safe | `filesystem_mutation: false`; index lives under AppData only |

## Non-goals (V1)

- Incremental / USN / MFT delta updates (`incremental_index: false`)
- Watching directories or background refresh
- Querying without a prior successful `index`
- Writing into the analyzed root
- Treemap, duplicates, MCP, product AI

## Storage layout

Indexes live under the per-user Local AppData tree (never next to the scanned data):

```text
%LOCALAPPDATA%\SpaceLens\indexes\<rootKey>\
    index.db              published, ready for query
    index.db.building     staging during rebuild (ephemeral)
    index.db.bak          temporary during publish swap
```

- **rootKey**: FNV-1a 64-bit hex of the case-folded normalized root path
- **Normalization**: drive roots and trailing-slash rules match safety policy
  (`normalizePathForPolicy`)
- **Schema version**: `index_schema_version = 1` (stored in `meta` and reported
  separately from CLI JSON `schema_version`)

## Schema (logical)

| Table | Role |
|-------|------|
| `meta` | `index_schema_version` and future key/value metadata |
| `roots` | One row per DB (V1: single root): path, key, counts, ISO timestamp, status |
| `entries` | Files and directories with size, times, classification, safety, reclaim fields |

Indexes cover `(root_id, kind, size)`, extension, write time, classification,
candidate strength, and path for deterministic filtered queries.

`roots` is **upserted**. Never `DELETE FROM roots` while `entries` exist —
`ON DELETE CASCADE` would wipe the body (fixed during V1 development).

## Build pipeline

```text
ScanEngine (live, read-only)
        ↓
DirectoryTree snapshot + classification / safety / reclaim analysis
        ↓
IndexStore::createStaging → index.db.building
        ↓
INSERT entries + writeRootMeta(status=ready)
        ↓
finalize statements, close DB
        ↓
publishIndexDatabase: live → .bak, staging → live, delete .bak
```

On cancel or failure before publish:

- Staging file is discarded
- Previous published `index.db` is left intact

## Query pipeline

```text
query <root> [filters]
        ↓
locateIndex → openRead(index.db)
        ↓
if missing → error index_not_found (exit 6)
        ↓
parameterized SQL filters + ORDER BY size DESC, path ASC + LIMIT
        ↓
JSON/human results with source: persistent_index and age_ms
```

There is **no silent live-scan fallback**. Agents must run `index` first.

## CLI surface

| Command | Purpose |
|---------|---------|
| `index <path>` | Full rebuild for one root |
| `index status <path>` | Existence, age, counts |
| `index list` | Enumerate published indexes under AppData |
| `query <path> …` | Filtered read-only query against the index |

Shared filters (with live `find`/`top` where applicable):

- `--files` / `--dirs`
- `--min-size SIZE` (binary 1024 units)
- `--ext EXT`
- `--older-than DAYS` (write-based)
- `--category` / classification string
- `--strength` (candidate strength)
- `--limit N`
- `--json`

## Capabilities

```json
{
  "persistent_index": true,
  "indexed_query": true,
  "incremental_index": false,
  "filesystem_mutation": false,
  "index_schema_version": 1
}
```

## Exit codes (index-related)

| Code | Meaning |
|------|---------|
| 0 | Success |
| 2 | Usage / bad args |
| 3 | Inaccessible root (for `index` when path missing) |
| 4 | Scan/index build failed |
| 5 | Cancelled |
| 6 | `index_not_found` (query/status against missing DB) |

## Implementation map

| Component | Path |
|-----------|------|
| SQLite amalgamation 3.53.4 | `third_party/sqlite/` |
| RAII DB/Stmt/Txn | `src/core/index/Sqlite.*` |
| Paths / publish | `src/core/index/IndexPaths.*` |
| Schema / open / meta | `src/core/index/IndexStore.*` |
| Build from scan | `src/core/index/IndexBuilder.*` |
| Query / status | `src/core/index/IndexQuery.*` |
| CLI wiring | `src/cli/Args.*`, `Commands.*`, `main.cpp` |
| Tests | `tests/test_index.cpp` |

## Safety reminders

- Index DBs are **metadata caches**, not backups of user data.
- Publishing rewrites only SpaceLens AppData files.
- Classification and reclaim fields in the index are **advisory** for review;
  they never become delete permission.
- Protected locations remain non-reclaim candidates in analysis; the index stores
  those fields for filtering, not for automated cleanup.
