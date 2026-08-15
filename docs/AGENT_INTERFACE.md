# SpaceLens agent interface

SpaceLens provides **deterministic storage evidence**. An external AI reasons
over that evidence. SpaceLens does not embed a model, accept prompts, or
mutate analyzed files.

```text
overview → opportunities → drill-down → specialized queries
```

Human-authorized Recycle Bin maintenance stays in the GUI. The CLI and the
stdio MCP adapter both report `filesystem_mutation: false` and
`read_only: true`. MCP tools are the same investigation, not a second
policy. See [`docs/MCP.md`](MCP.md).

## Why SpaceLens for AI agents

Listing tens of thousands of files and asking a model to "find waste" is
slow, expensive, and unsafe. SpaceLens scans or queries an index once, then
returns a **bounded, ranked, structured** picture:

- what is consuming space
- which areas deserve human review first
- how many bytes each area involves
- why each item was surfaced
- what is unknown or protected

The agent should normally consume tens of JSON objects, not tens of thousands
of paths.

## Storage investigation workflow

For "My drive is almost full. Find what I should review.":

```powershell
spacelens capabilities --json
spacelens index list --json                # discover published snapshots
spacelens index status D:\ --json          # optional: one-root details
spacelens overview D:\ --from-index --json # reuse a chosen snapshot
spacelens opportunities D:\ --from-index --json
spacelens query D:\ --dirs --under D:\Projects\app --limit 20 --json
spacelens duplicates D:\ --json            # if an index exists
```

The same workflow through MCP (`spacelens-mcp.exe`):

```text
storage_capabilities
storage_index_status      { "path": "D:\\" }
storage_overview          { "path": "D:\\", "source": "live_scan" }
storage_opportunities     { "path": "D:\\" }
storage_query             { "path": "D:\\", "object_type": "directory",
                            "under": "D:\\Projects\\app", "limit": 20 }
```

Start with `spacelens index list --json` to see which roots have a
published snapshot, how old each is, and whether it is usable
(`status=ready`, current `index_schema_version`). Then choose a root
and run `overview` / `opportunities` / `query`. `index list` does not
refresh or live-scan. MCP `storage_index_status` still requires a
known root — there is no list-indexes MCP tool.

Prefer a published index when `index list` / `index status` /
`storage_index_status` shows an acceptable `freshness` / `age_ms` and
`status`. Exact is not live: age is the published snapshot age (latest
successful generation), not proof the filesystem is unchanged. Indexed
commands take `--from-index` (overview/opportunities) or `source:
persistent_index`. `query` / `storage_query` and `duplicates` /
`storage_duplicates` are index-only. They never silently refresh.
Optional `--max-index-age-seconds` / `max_index_age_seconds` fails
closed (exit 4 / domain error) before top-N or aggregates. No default
max-age. Do not invent `fresh: true`.

```powershell
spacelens overview D:\ --from-index --json
spacelens opportunities D:\ --from-index --json
spacelens overview D:\ --from-index --max-index-age-seconds 3600 --json
```

Missing index → exit code **6**. Snapshot too old / freshness unknown →
exit **4** (`index_too_old` / `index_freshness_unknown`); JSON still
includes `index.freshness`. Do not treat indexed evidence as live
filesystem truth.

## Five core questions

### What is consuming the most space?

```text
spacelens overview <path> --json
```

Read `summary.logical_bytes`, `largest_directories`, and `largest_files`.
Each consumer includes classification, reclaimability, and location safety.
A 40 GB VM image can appear here without being an opportunity. Live
`overview` also includes a compact `opportunity_summary` (category
totals). Indexed overview leaves that array empty so it is not a second
opportunities dump.

`scan` + `top --dirs` + `top --files` still work but each command rescans.
`overview` is one scan.

### What are the strongest review opportunities?

```text
spacelens opportunities <path> --json
```

Read `groups` first (aggregate bytes by deterministic class), then
`opportunities` (ranked items). Fields:

| Field | Meaning |
| --- | --- |
| `classification` | What it appears to be |
| `reclaimability` | Whether that class is typically regenerable |
| `location_safety` | Where it lives (Protected / Sensitive / Ordinary / Unknown) |
| `candidate_strength` | Review priority (Strong / Moderate / ReviewOnly / None) |
| `reason_codes` | Stable machine codes (`developer_dependency`, `old_large_file`, …) |
| `evidence` | Compact `{ecosystem, marker}` that fired the rule |
| `logical_bytes` | Bytes involved in this item |
| `overlapped` | Descendant already covered by a selected ancestor directory |

`summary.unique_review_bytes` is the non-overlapping sum of selected
candidates **before** `--limit`. It is not guaranteed freed space and is
not the sum of the returned page when `truncated` is true.

Indexed top-N is exact across the **whole published index**. Filters
(classification, `--under`, min-size, age, object type) run in SQL
**before** `LIMIT`. `--limit 20` means the best 20 matching rows by
`opportunity_rank_v2`, not the first 20 rows SQLite happens to store.
A stronger candidate at row 425,000 still wins.

`unique_review_estimated` is true only when overlap-aware byte addition
overflows `uint64`. It never means top-N is approximate, and it is no
longer set merely because more than 50,000 rows match. `unique_review_bytes`
is exact logical review bytes on the published index (nested directory
candidates are not double-counted). It is not guaranteed reclaimable
disk space and is not live-filesystem truth if the index is stale.

There is no `safe_to_delete` and no `potential_reclaim_bytes` headline.

Ranking policy `opportunity_rank_v2`: candidate strength DESC, confidence
DESC, logical bytes DESC, normalized path ASC. Regenerable classes need
Medium or High confidence to enter the list (name-only `temp` / `cache` /
`build` without project context is excluded). High-confidence regenerable
items ≥ 10 MB rank Moderate even when recent.

Filter: `spacelens opportunities <path> --classification BuildArtifact`.
`--under PATH` restricts live and indexed opportunities to that subtree.
An unrecognized class name matches nothing (it does not silently become
`Unknown`). Duplicates are a separate command and are **not** hashed from
`opportunities`.

Default `--limit` is 20. Default `--min-size` is 1 MB. `truncated` is true
when more candidates existed.

### Which developer / generated / cache areas are large?

Use `opportunities` groups `developer_dependencies`, `generated_outputs`,
`package_cache`, `ide_cache`, and `temporary_data`. Or drill with:

```text
spacelens query <indexed-root> --dirs --classification DependencyDirectory --json
spacelens find <path> --classification BuildArtifact --json
```

Classifications are existing SpaceLens rules (`node_modules`, CMake
markers, Rust `target` next to `Cargo.toml`, .NET `bin`/`obj` next to a
project file, Python `pyvenv.cfg`, `.cache`, …). A folder merely named
`build` or `temp` is not enough. Filename guesses are not invented at
query time. Group objects include `strongest_candidate_strength`.
Overlapped descendants carry `nested_overlap` and are omitted from unique
bytes.

### Which old large files deserve review?

```text
spacelens opportunities <path> --older-than 90 --min-size 10MB --json
spacelens find <path> --min-size 10MB --older-than 90 --json
spacelens query <indexed-root> --files --min-size 10MB --older-than 90 --json
```

Age is write-based (directory = newest descendant write). Last access is
advisory and cannot produce Strong. Old ≠ unused.

### Which verified duplicates may represent wasted storage?

```text
spacelens duplicates <indexed-root> --json
```

Same-size files are candidates only. Verification is full SHA-256.
Hard-link aliases of one identity are the same file: redundant logical
bytes for that identity are 0. `summary.potential_redundant_logical_bytes`
counts extra **distinct identities** in a verified group, not
`path_count * size`. `--delete` / `--keep-one` are unknown options.

## Drill-down

After an opportunity such as `D:\Projects\app\node_modules`:

```text
spacelens overview D:\Projects\app\node_modules --json
spacelens top D:\Projects\app --dirs --limit 20 --json
spacelens query D:\ --dirs --under D:\Projects\app --limit 20 --json
```

`--under` is index-only and restricts `query` to that path and descendants.
Live drill-down is a scan of the smaller path (not a second full-volume scan).

## Evidence model

These four analysis concepts stay separate:

| Concept | Field | Not the same as |
| --- | --- | --- |
| What is it? | `classification` | permission to delete |
| Is it typically regenerable? | `reclaimability` | "this user does not need it" |
| Where is it? | `location_safety` | reclaimability |
| How strongly should a human review it? | `candidate_strength` | authorization to act |

`filesystem_mutation` is a fifth, always-false CLI capability. It is not
an analysis score.

Protected locations never become opportunities. Sensitive locations cap
strength at Moderate. User / unknown / archive content is never Strong just
because it is large or old. Old large archives/installers may appear as
review items with `old_large_archive` / `old_large_installer`; that is
not a delete recommendation. Downloads location uses the Windows Known
Folder, not an English path hardcode.

## Live vs indexed evidence

| `source` | Meaning |
| --- | --- |
| `live_scan` | Result of a scan that just ran |
| `persistent_index` | Published snapshot; see `index.age_ms` / `indexed_at` |

`index status --json` reports existence, age, counts, and
`incremental_refresh` (supported / full rebuild required). The agent
decides whether to refresh or rebuild. Query never auto-refreshes.

## Bounded results

| Command | Default bound |
| --- | --- |
| `overview` | 10 directories + 10 files |
| `opportunities` | 20 items |
| `top` / `find` / `query` | 20 |
| `duplicates` | verified groups only; `--min-size` default 1 MB; optional derived hash cache in AppData |

JSON uses integer bytes, stable enums, UTF-8 paths, and `schema_version: 1`.
Additive fields are forward-compatible. Diagnostics go to stderr.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success (including zero matches) |
| 1 | Internal error |
| 2 | Usage / unknown command |
| 3 | Inaccessible or missing root |
| 4 | Scan / query / index failure |
| 5 | Cancelled |
| 6 | Published index not found |

Cancelled JSON is still a single object (`state: cancelled`). Partial
duplicate groups are marked; the index is not corrupted.

## Safety boundary

The CLI cannot delete, recycle, restore, move, rename, purge, or empty the
Recycle Bin. `capabilities --json` must keep:

```json
"read_only": true,
"filesystem_mutation": false
```

An agent that wants files removed must explain the evidence to a human. The
human uses Cleanup Review in the GUI and confirms **Move eligible files to
Recycle Bin**.

## Example external-agent workflow

User: "My D drive is almost full."

1. `spacelens overview D:\ --json` — total size and top consumers
2. `spacelens opportunities D:\ --json` — groups + ranked review list
3. `spacelens query D:\ --dirs --under <top-opportunity> --json` — look inside
4. `spacelens duplicates D:\ --json` if an index exists
5. Explain to the human what is large, what looks regenerable, what is old,
   what is duplicated, and what is uncertain
6. Do **not** delete anything

## Storage Intelligence V2

Coverage and ranking improvements over V1. Schema stays `schema_version: 1`
(additive). Version stays 0.1.3.

### What improved

- Sibling-aware developer classification (Rust `target`, .NET `bin`/`obj`,
  Python venv) instead of leaf-name-only guesses
- Name-only `temp` / `tmp` / `cache` / `build` is no longer enough
- High-confidence regenerable items ≥ 10 MB rank Moderate even when recent
- Ranking is `opportunity_rank_v2`: strength, then confidence, then bytes
- Compact `evidence {ecosystem, marker}` and extra reason codes
- Live `overview.opportunity_summary` category totals
- `--classification` / MCP `classification` on opportunities

### Categories and evidence

| Class | Typical evidence |
| --- | --- |
| `DependencyDirectory` | `node_modules`; `pyvenv.cfg`; `.venv`/`venv` next to a Python project marker |
| `BuildArtifact` | CMake cache+files; Rust `target` + `Cargo.toml`/`CACHEDIR.TAG`; .NET `bin`/`obj` + project file; `__pycache__` |
| `PackageCache` | `.nuget` / `.m2` / `.gradle` / pip-cache names; `packages` under NuGet/.NET context |
| `IdeCache` | `.vs` / `.idea` / `.vscode` |
| `TemporaryData` | path under `GetTempPathW`; `.cache`; weak name-only `temp`/`tmp`/`cache` (Low, excluded) |
| `LogData` | `log`/`logs` directory or `.log` file |
| `Archive` | zip family, `iso`/`img`, `msi`/`msu`/`cab` — review only when old+large |
| `DownloadedAiModel` | gguf/onnx/pt/pth/safetensors/ckpt |
| `ApplicationData` | `.git` |
| `UserData` / `Unknown` | media/docs or no rule |

### No deletion

Classification is not permission. CLI and MCP stay `read_only: true` and
`filesystem_mutation: false`. There is no `safe_to_delete`,
`should_delete`, or `deletion_recommended`.

### Aggregation

`unique_review_bytes` and group `logical_bytes` skip `overlapped`
descendants (`nested_overlap`). A mixed parent such as `D:\Projects` is
not itself an opportunity unless a classifier matches it. Groups include
`strongest_candidate_strength`.

### Known limitations

- Custom build systems without CMake/.NET/Rust/Node/Python markers stay
  Unknown
- Application-specific caches not in the table are not invented
- Write timestamps are advisory; LastAccess cannot produce Strong
- Indexed opportunities use stored classification; ecosystem may be
  derived from `rule_id` only
- `opportunities` does not hash files; call `duplicates` separately
- Persistent hash cache is not a package-cache classification

## Related

- [`docs/CLI.md`](CLI.md) — command contract
- [`docs/INDEX.md`](INDEX.md) — persistent index
- [`docs/DUPLICATES.md`](DUPLICATES.md) — verification and hard links
- [`docs/SAFETY.md`](SAFETY.md) — mutation boundary
