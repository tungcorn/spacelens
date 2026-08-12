# Persistent Index (V1 + V2)

SpaceLens builds a **read-only SQLite index** of a scanned root and answers
repeated size/classification queries without rescanning the filesystem.

**Incremental Index V2** can refresh that index from the NTFS **USN Change
Journal** when a volume handle can be opened read-only and a checkpoint is ready.
It never creates, resizes, deletes, or reconfigures the journal, and never mutates
analyzed user data.

## Goals

| Goal | Behavior |
|------|----------|
| Full rebuild | `index <root>` scans once and publishes `index.db` |
| Fast re-query | `query` reads only the published DB |
| Safe rebuild | Staging DB + atomic publish; cancel/fail discards staging only |
| Incremental refresh | `index refresh <root>` applies USN deltas when checkpoint is ready |
| Explicit failure | Missing index → exit **6**; no live query fallback |
| Discontinuity | Journal/volume mismatch → `full_rebuild_required` (no guessing) |
| Agent-safe | `filesystem_mutation: false`; AppData-only index writes |

## Non-goals

- Auto-refresh on `query` (refresh is always explicit)
- Creating or configuring the USN journal
- Writing into the analyzed root
- GUI polish, treemap, MCP, product AI, deletion/movement
- MFT-based initial scan (future)

## Storage layout

```text
%LOCALAPPDATA%\SpaceLens\indexes\<rootKey>\
    index.db              published, ready for query
    index.db.building     staging during full rebuild (ephemeral)
    index.db.bak          temporary during publish swap
```

- **rootKey**: FNV-1a 64-bit hex of the case-folded normalized root path
- **index_schema_version**: **2** (CLI JSON still uses `schema_version: 1` for the
  response envelope)

## Schema (logical)

| Table | Role |
|-------|------|
| `meta` | `index_schema_version` and key/value metadata |
| `roots` | One row per DB: path, key, counts, ISO timestamp, status |
| `entries` | Files/directories + classification/safety/reclaim fields; V2 adds `file_id`, `parent_file_id` (NTFS FRN) |
| `refresh_checkpoint` | Volume identity, USN journal id, next USN, method, status |

`roots` is **upserted**. Never `DELETE FROM roots` while `entries` exist —
`ON DELETE CASCADE` would wipe the body.

### Migration

Opening a V1 database for write migrates in place:

1. `ALTER TABLE entries ADD COLUMN file_id / parent_file_id` (default 0)
2. Create `refresh_checkpoint` if missing
3. Set `index_schema_version = 2`

Old indexes remain queryable; incremental refresh needs a new full `index` so
FRNs and a checkpoint are populated.

## Full build pipeline

```text
ScanEngine (live, read-only)
        ↓
DirectoryTree + classification / safety / reclaim
        ↓
IndexStore staging → INSERT entries (with file_id when available)
        ↓
writeRefreshCheckpointAfterFullBuild (best-effort USN cursor)
        ↓
publishIndexDatabase (atomic)
```

Checkpoint `status` after full build:

| status | Meaning |
|--------|---------|
| `ready` | Journal open + query succeeded; `usn_journal_id` / `next_usn` stored |
| `access_denied` | Volume open / USN IOCTL denied (common without admin/backup privilege) |
| `journal_not_active` | NTFS volume has no active journal (we never create one) |
| `unsupported_filesystem` | Not NTFS |
| `unavailable` | Other / unknown |

Full build **never fails** solely because the journal is unavailable.

## Incremental refresh pipeline

```text
index refresh <root>
        ↓
probeIncremental: index exists? checkpoint ready? live USN ok?
        ↓
read USN records since next_usn (FSCTL_READ_USN_JOURNAL only)
  → outNextUsn = driver READ continuation USN (never record.usn+1)
        ↓
coalesce by FRN → delete / upsert under root filter
        ↓
recompute dirty directory aggregates + ancestors
        ↓
advance checkpoint.next_usn = continuation (same SQLite transaction)
```

- **Atomic**: checkpoint advances only if the delta transaction commits.
- **Cancel / fail**: previous index + previous checkpoint remain valid.
- **Cursor lifecycle**: `next_usn` is the exclusive start for the next
  `FSCTL_READ_USN_JOURNAL`. It is always a driver-issued continuation USN or the
  live journal `NextUsn`. USN values are journal **offsets**, not dense integers —
  inventing `record.usn + 1` produces misaligned `StartUsn` →
  `ERROR_INVALID_PARAMETER` → `history_lost` / `full_rebuild_required` with
  `journal_records_seen=0` on the *next* refresh (multi-batch failure mode).
- **Empty tail**: `startUsn == NextUsn` is `already_current` / Supported — not a
  discontinuity. Empty ReasonMask-filtered buffers still advance via the driver
  continuation; the read loop does not stop mid-journal solely because a buffer
  had zero matching records.
- **Subdirectory roots**: USN is volume-wide; records outside the indexed root are
  ignored (`pathIsUnderRoot`). Moves out of root delete the old entry. Directory
  renames rewrite descendant paths and dirty both old and new parents.
- **Missing parent under root**: `full_rebuild_required` (`missing_parent`) rather
  than inventing orphans.
- **No auto-refresh**: `query` never calls refresh.
- **Multi-batch / restart**: the same published index must accept refresh A → B →
  C and a process-restart refresh from the persisted checkpoint. Verify with
  `scripts/verify-usn-refresh.ps1` (gates overall `outcome` on multi-batch +
  restart, not only single-mutation parity).
- **Aggregates**: dirty directories recompute **deepest-first** (child `recursive_size`
  before parent). Reverse order left file counts correct but `logical_bytes` lag.
- **Same-window parents**: if a child appears before its new parent in the FRN
  map, parents under the root are materialized from live disk (`ensureDirUnderRoot`)
  rather than immediately `missing_parent`.

### Capability reasons (`incremental_refresh.state`)

| State | Typical reason |
|-------|----------------|
| `supported` | Checkpoint ready; journal matches |
| `access_denied` | Cannot open volume for USN (needs elevation or SeBackupPrivilege) |
| `journal_not_active` | No journal on volume |
| `journal_changed` | UsnJournalID mismatch |
| `history_lost` | Cursor below LowestValidUsn / entries deleted |
| `volume_changed` | Volume serial mismatch |
| `unsupported_filesystem` | Not NTFS |
| `unavailable` / `needs_full_rebuild` | No ready checkpoint or other discontinuity |

Agents should treat any non-supported state as **run `index` (full rebuild)**.

## CLI surface

| Command | Purpose |
|---------|---------|
| `index <path>` | Full rebuild + best-effort checkpoint |
| `index refresh <path>` | USN incremental apply when possible |
| `index status <path>` | Index age/counts + `incremental_refresh` block |
| `index list` | List published indexes under AppData |
| `query <path> …` | Read-only SQL against published DB |

Capabilities advertise:

```json
"incremental_index": true,
"filesystem_mutation": false,
"index_schema_version": 2
```

## GUI — Index Browser V1

The GUI hosts an **Indexed** tab alongside **Live Scan**:

| Piece | Role |
|-------|------|
| `IndexCatalog` (core) | List/summarize roots, map freshness, build query specs |
| `IndexSession` (app) | Async full rebuild / USN refresh (ScanSession pattern) |
| `IndexBrowserPage` (ui) | Roots list, filters, query hits, inspector, review queue |

Rules:

- Qt code calls **core APIs only** (`listIndexSummaries`, `queryIndex`,
  `probeIncremental` via catalog, `refreshIndex`, `buildIndexForRoot`) — no SQLite
  in the UI layer.
- Queries always declare **snapshot** source; inspector shows age / indexed-at.
- Cleanup Review items from the index carry `source=persistent_index` and age.
- Refresh / rebuild are **explicit** buttons; cancel is cooperative.
- No treemap, delete, move, MCP, or product AI in this milestone.

Freshness labels (display):

| Label | Meaning |
|-------|---------|
| Fresh snapshot | Index exists; age under soft threshold; USN not advertised |
| Aged snapshot | Exists; older than soft threshold |
| Refresh available | USN incremental supported |
| Incremental unavailable | Snapshot ok; USN blocked (access, journal, FS) |
| Full rebuild required | Journal/volume discontinuity |
| Missing / Error | No DB or open failure |

## Safety

- **Analyzed filesystem**: never deleted/moved/rewritten by index or refresh.
- **USN**: only `FSCTL_QUERY_USN_JOURNAL` and `FSCTL_READ_USN_JOURNAL`.
  Never create/delete/extend/configure journal.
- **Volume handle**: `GENERIC_READ` / read attributes only; optional
  `SeBackupPrivilege` enable if already present on the token.
- **Index files**: AppData only.

## Platform modules

| Module | Role |
|--------|------|
| `VolumeHandle` | Volume identity + read-only open |
| `FileIdentity` | FRN via `GetFileInformationByHandle`; path via `OpenFileById` |
| `UsnJournal` | Capability model + read-only journal reader |
| `IndexRefresh` | Probe, coalesce, apply, checkpoint |

## Exit codes (index-related)

| Code | Meaning |
|------|---------|
| 0 | Success (`index`, `query`, successful refresh / already current) |
| 2 | Usage |
| 3 | Path inaccessible |
| 4 | Index build / refresh failed |
| 5 | Cancelled |
| 6 | Index not found (`query` / refresh) |

`index refresh` returning `full_rebuild_required` is a soft, expected outcome
when the journal is unavailable — exit non-zero with structured reason, index
still queryable.
