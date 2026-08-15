# Persistent Index (V1 + V2 + V3)

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
- Product AI, deletion/movement, or MCP mutation (MCP may *read* a
  published index via `storage_query` / `storage_index_status` and must
  never refresh it)
- MFT-based initial scan (future)
- Fuzzy/semantic search, watch mode, new USN optimizations

## Storage layout

```text
%LOCALAPPDATA%\SpaceLens\
    state.db              durable Cleanup Review (independent; see CLEANUP_REVIEW.md)
    indexes\<rootKey>\
        index.db              published, ready for query
        index.db.building     staging during full rebuild (ephemeral)
        index.db.bak          temporary during publish swap
```

`SPACELENS_DATA_ROOT` overrides the AppData root (indexes, review state,
and hash cache). Tests and verify scripts set it to a temp directory so
they never open the developer’s real `%LOCALAPPDATA%\SpaceLens`.

`state.db` is not an index. Rebuild, USN refresh, and deleting `index.db` must
not touch review rows.

- **rootKey**: FNV-1a 64-bit hex of the case-folded normalized root path
- **index_schema_version**: **3** (CLI JSON still uses `schema_version: 1` for the
  response envelope). V3 adds physical allocation / hard-link columns and
  meta `physical_accounting`.

## Schema (logical)

| Table | Role |
|-------|------|
| `meta` | `index_schema_version` and key/value metadata |
| `roots` | One row per DB: path, key, counts, ISO timestamp, status |
| `entries` | Files/directories + classification/safety/reclaim fields; V2 adds `file_id`, `parent_file_id` (NTFS FRN); V3 adds `allocated_bytes` (NULL when unknown), `allocation_known`, `hard_link_count`, `observed_link_count`, `sparse`, `compressed`, `volume_serial`, `hard_link_coverage` |
| `refresh_checkpoint` | Volume identity, USN journal id, next USN, method, status |

`roots` is **upserted**. Never `DELETE FROM roots` while `entries` exist —
`ON DELETE CASCADE` would wipe the body.

### Migration

Opening a V1 database for write migrates in place:

1. `ALTER TABLE entries ADD COLUMN file_id / parent_file_id` (default 0)
2. Create `refresh_checkpoint` if missing
3. Set `index_schema_version = 2`

Opening a V2 database for write migrates to V3 in place:

1. `ALTER TABLE entries ADD COLUMN` physical fields (`allocated_bytes` NULL,
   `allocation_known` 0, `hard_link_count` / `observed_link_count` 0,
   `sparse` / `compressed` 0, `volume_serial` 0, `hard_link_coverage`
   `unknown`)
2. Set `index_schema_version = 3`
3. Do **not** set meta `physical_accounting=1`

Migrated v2 rows stay queryable for overview / opportunities / query /
breakdown. `reclaim-plan --source persistent_index` fail-closes unless
meta `physical_accounting=1`. That flag is set only after a **full** v3
`index` finalize (or a refresh of an index that already had the flag).
An incremental refresh of a migrated v2 must not claim physical
accounting is complete.

Old indexes remain queryable; incremental refresh needs a new full `index` so
FRNs and a checkpoint are populated. Physical reclaim-plan on the index
needs a new full `index` so allocation / hard-link evidence is populated.

## Full build pipeline

```text
ScanEngine (live, read-only)
        ↓
DirectoryTree + classification / safety / reclaim
        ↓
IndexStore staging → INSERT entries (with file_id + physical columns)
        ↓
finalizePhysicalAccounting (unique allocated rollup; meta physical_accounting=1)
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
| `index list` | Compact published-index catalog (status + freshness; no refresh) |
| `query <path> …` | Read-only SQL against published DB |
| `opportunities <path> --from-index` | Exact top-N review candidates across the whole index |
| `breakdown <path>` | Index-only file-logical-byte mix by classification, extension, last-write age |

Indexed `opportunities` does **not** prefetch a 200-row prefix and rank
in C++. `queryIndexedOpportunities` applies the inclusion predicate and
`opportunity_rank_v2` (`ORDER BY` strength, confidence, size, path) in
SQL, then `LIMIT N+1`. Classification, `--under`, min-size, and age
filters run before that limit.

Overlap-aware `unique_review_bytes` is an **exact** non-overlapping sum
of matching indexed `logical_bytes` for the same inclusion predicate.
A second SQL stream orders by a tree-preorder key — separators `/` and
`\` are rewritten to `char(1)` before `COLLATE NOCASE`, then `id ASC` —
and feeds a bounded ancestor stack (memory scales with nesting depth
and group count, not match count). Raw `ORDER BY path` is not a tree
walk (`'2' < '\'`, so `foo2` would sort before `foo\bar` and pop the
ancestor too early). Directory opportunities cover descendants; files
never do. Near-prefix siblings (`foo` vs `foobar`) are not descendants. Group `logical_bytes` use that same **global** overlap
(so `sum(groups)` equals the headline except for ungrouped rows);
`item_count` still includes overlapped members. Category totals are
therefore not independently additive across categories.

`unique_review_bytes` is exact for the **published index evidence**, not
a guaranteed freed-space figure. Sparse files, compression, hardlinks,
allocation units, and Recycle Bin behavior can make live disk change
differ. Exact is not live. A stale index is still reported exactly as
stored.

### Snapshot freshness (Honesty V1)

Indexed results report the age of the **latest successfully published
generation**, not `index.db` mtime and not a live-filesystem probe.

- Published timestamp = `refresh_checkpoint.last_refresh_at_ticks` when
  non-zero, otherwise `roots.indexed_at`. Incremental refresh keeps
  `roots.indexed_at` at the last full build and only advances the
  checkpoint after a successful commit. A failed refresh does not move
  the published timestamp.
- `index.age_ms` / `index.indexed_at` are derived from that published
  snapshot. Additive `index.freshness` names the basis
  (`published_snapshot`), `age_state` (`known` / `unknown` /
  `clock_skew`), `publish_kind`, and `age_seconds` when known. There is
  no `fresh: true`.
- Optional `--max-index-age-seconds N` / MCP `max_index_age_seconds`
  fails closed **before** Query A/B / top-N / breakdown aggregation:
  `age <= N` is allowed; older, unknown, or clock-skew is rejected
  (`index_too_old` or `index_freshness_unknown`, exit **4**). No default
  policy. No auto-refresh. No live fallback. `index status` reports
  honest age and never applies the gate. `duplicates` is exempt (index
  candidates, then live hash verify). `breakdown` is index-only and
  uses the same published-snapshot gate; it never scans or refreshes.

`breakdown` aggregates **files only** (`kind = 0`, `size_bytes`). A
directory's `recursive_size` is never added. The three dimensions are
independent: stored classification, stored extension (top-N plus exact
`other`), and last-write age buckets. Totals must reconcile. Memory
does not scale with file count. Logical bytes are index namespace
size, not physical usage or reclaimable space.
- `index list --json` is the agent catalog entry: every usable published
  root includes the same `freshness` object (one captured `now` per
  invocation). Status is `ready` / `incompatible` / `unavailable` /
  `corrupt`. Listing does not migrate V1 indexes, probe USN, or scan the
  analyzed filesystem. Broken entries stay visible; they do not fail the
  catalog. `index list` and `index status` agree on published timestamp
  and freshness for current-schema ready roots.

`unique_review_estimated` is true only when checked byte addition
overflows `uint64` (the public value saturates at `2^64-1` and is not
a wrapped smaller total). It never means top-N is approximate and is
not used merely because the match count crossed 50,000.

Queries never refresh or rebuild the index. Top-N and the aggregate
stream share one read connection and `BEGIN DEFERRED` snapshot.

UNC roots are not a dedicated indexed-opportunity target; path helpers
are drive-letter / backslash oriented. Reparse targets are not live-
resolved during indexed aggregation.

Capabilities advertise:

```json
"incremental_index": true,
"filesystem_mutation": false,
"index_schema_version": 2
```

## GUI — Index Browser V2 (storage discovery)

The GUI hosts an **Indexed** tab alongside **Live Scan**. The Indexed tab is the
main **fast storage-discovery** surface: choose a published root, pick a discovery
mode, search/filter/sort, inspect, then Open / Reveal / Copy / Add to Cleanup Review.

| Piece | Role |
|-------|------|
| `IndexCatalog` (core) | List/summarize roots, freshness, discovery presets → `IndexQuerySpec` |
| `IndexQuery` (core) | Typed filters, text search, sort, browse path; all SQL stays here |
| `IndexSession` (app) | Async full rebuild / USN refresh (ScanSession pattern) |
| `IndexBrowserPage` (ui) | Roots, header, overview, treemap, presets, filters, table, breadcrumb, inspector, review |
| `TreemapLayout` (core) | Squarified layout + “Other” aggregation (Qt-free, unit-tested) |
| `IndexOverview` (core) | Hierarchy children + non-overlapping overview metrics |
| `TreemapWidget` (ui) | Native QPainter treemap; no SQL in paintEvent |

### Practical workflow

```text
choose indexed root
    ↓
see storage overview + squarified treemap (immediate children, logical size)
    ↓
choose discovery mode (Largest / Old & Large / Developer / Reclaim / Custom)
    ↓
search / filter / sort (typed IndexQuerySpec — no raw SQL in Qt)
    ↓
inspect interesting item (classification, rule id, reclaim strength)
    ↓
Open / Reveal / Copy  ·  Add to Cleanup Review
    ↓
drill into a folder via treemap double-click or breadcrumb — still index-only
```

### Storage overview and treemap (V1)

| Concept | Behavior |
|---------|----------|
| Scope | Immediate children of the **current browsed location** (root or folder) |
| Weight | Logical bytes: file `size_bytes`, directory `recursive_size` |
| Partition | Direct-file sizes + child-directory recursive sizes partition the location total without double-counting nested paths |
| Overview intelligence | **Counts** among direct children (developer / reclaim / old&large) — never a sum of recursive directory sizes labeled “reclaimable bytes” |
| Layout | Squarified treemap in core (`layoutSquarified`); Qt paints prepared rectangles only |
| “Other” | Visualization aggregate for the long tail of tiny children; not a filesystem path, not a cleanup candidate, not a DB row |
| Filters | Discovery presets filter the **table**; treemap stays physical hierarchy of the location |
| Navigation | Double-click directory rectangle → `browsePath` / breadcrumb; no live rescan |
| Terminology | **Indexed logical size** — not physical size-on-disk |

### Discovery presets (`IndexDiscoveryPreset`)

| Preset | Query semantics |
|--------|-----------------|
| Largest | Files + folders, sort size DESC |
| Old & Large | Default min size 100 MiB + activity older than 90 days (write/descendant), size DESC |
| Developer Storage | Classification IN BuildArtifact, DependencyDirectory, PackageCache, IdeCache, DownloadedAiModel, LogData |
| Reclaim Candidates | Strength IN Strong, Moderate; sort strength then size |
| Custom | User filters only |

Presets call `applyDiscoveryPreset` / `makeDiscoveryQuery`. GUI kind/sort/search
overrides still win when the user changes them.

### Search and filters

- **Search:** case-insensitive substring (`LIKE %needle% ESCAPE '\'`) on `name`,
  `path`, and `extension`. Empty query disables search. Not fuzzy/semantic.
- **Filters:** type (all/files/folders), min size, activity age, extension,
  classification, reclaim strength, limit (default 200).
- **Sort:** size, name, activity (last write), classification, reclaim strength.
- **Browse path:** double-click a folder or use the breadcrumb to set
  `browsePath` → immediate children via `parent_id` (index data only).
- **Result summary:** `matched_items`, `matched_logical_bytes`, `returned_items`,
  `query_elapsed_ms` — UI shows “Showing N of M matches” and never pretends the
  page is the full set.

### Rules

- Qt code calls **core APIs only** (`listIndexSummaries`, `queryIndex`,
  `makeDiscoveryQuery`, `probeIncremental` via catalog, `refreshIndex`,
  `buildIndexForRoot`) — **no SQLite in the UI layer**.
- Queries always declare **snapshot** source; inspector shows age / indexed-at /
  matched rule id when stored.
- Cleanup Review items from the index carry `source=persistent_index`, age, and
  captured classification metadata. They are stored in independent
  `%LOCALAPPDATA%\SpaceLens\state.db` (`review_schema_version = 1`). Rebuilding,
  refreshing, or deleting an index must not erase or rewrite those rows. See
  [`CLEANUP_REVIEW.md`](CLEANUP_REVIEW.md).
- Refresh / rebuild are **explicit**; full rebuild is never silent after
  `full_rebuild_required`. Incremental USN may require elevation — the GUI does
  not spam UAC; it states incremental unavailable and offers Rebuild.
- Open / Reveal check live path existence; missing paths show a stale-snapshot
  message. Multi-select supports Copy Paths and Add to Review only (no bulk Open).
- Treemap is navigation/inspection only. No delete, move, MCP, or product AI.

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
| 4 | Index build / refresh failed, or max-age gate (`index_too_old` / `index_freshness_unknown`) |
| 5 | Cancelled |
| 6 | Index not found (`query`, `duplicates`, `--from-index`, refresh) |

`query --under PATH` restricts hits to that path and its descendants
(`path = ? OR path LIKE ? ESCAPE '\'`). The bound LIKE pattern escapes
`\`, `%`, and `_` so a Windows prefix such as `D:\Projects\app` is a
literal path, not an escape sequence.

`index refresh` returning `full_rebuild_required` is a soft, expected outcome
when the journal is unavailable — exit non-zero with structured reason, index
still queryable.
