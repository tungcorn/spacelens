# SpaceLens Safety Architecture

## Product safety principle

SpaceLens helps humans and agents **find, understand, inspect, and plan** storage
cleanup. It does **not** grant automated authority to destroy data.

> **AI recommendation is not filesystem permission.**

> **SpaceLens CLI is read-only by design.**

Whenever convenience conflicts with filesystem safety, choose safety.
Whenever AI confidence conflicts with deterministic filesystem policy, choose
deterministic policy.
Whenever a destructive action can wait for explicit human review, delay it.

## Two consumers, two trust levels

| Consumer | Interface | Mutation |
|----------|-----------|----------|
| AI agents / scripts | `spacelens.exe` (CLI) | **None** — observation and query only |
| Humans | `spacelens-gui.exe` | Inspection, cleanup review, and **human-authorized Recycle Bin** only |

The agent-facing CLI must remain safe to grant to a coding agent without
implicitly granting file-deletion or file-movement capability.

## CLI: read-only by design

The primary executable:

```text
build/cli/spacelens.exe
```

exposes deterministic **scan / query / classify / report / index / duplicates**
operations only.

It must **not** register commands such as:

```text
delete  remove  rm  move  cleanup execute  purge  wipe  dedupe  keep-one
recycle  maintenance
```

There are no hidden aliases for destructive operations.
There is no generic shell-execution feature.
LLM output must never be turned into a filesystem command automatically.

Persistent index build, refresh, and query write **only** SpaceLens metadata under
`%LOCALAPPDATA%\SpaceLens\`. They never delete, move, or rewrite files under
the analyzed root. `query` is read-only against the index database; a missing
index fails closed (exit 6) instead of falling back to a silent live path that
could be mistaken for authority over live data.

USN incremental refresh uses only `FSCTL_QUERY_USN_JOURNAL` and
`FSCTL_READ_USN_JOURNAL`. SpaceLens never creates, deletes, extends, or
reconfigures the change journal. Volume handles are opened read-only. When the
journal cannot be opened (typical without elevation), refresh fails closed with
`access_denied` / `full_rebuild_required` rather than inventing deltas.

Agents can discover this contract:

```text
spacelens capabilities --json
```

with:

```json
"filesystem_mutation": false
```

### GUI Index Browser (V2) + Treemap V1

The Indexed tab is discovery, visualization, and planning only:

- No Delete / Move / Rename controls.
- Treemap interaction is navigation/inspection only (hover, select, drill-down).
- The “Other” treemap bucket is a visualization aggregate — never a cleanup target.
- Cleanup Review is a durable planning queue in
  `%LOCALAPPDATA%\SpaceLens\state.db` (`source=persistent_index` preserves
  snapshot age and classification metadata). Index rebuild/refresh does not
  rewrite review evidence. Recycle Bin maintenance is a separate, confirmed
  GUI path — see [`docs/MAINTENANCE.md`](MAINTENANCE.md).
- Open / Reveal / default-app launch are human-initiated shell actions on paths
  the user selected; missing paths surface a stale-snapshot message rather than
  rewriting the index.
- Incremental refresh does not self-elevate in a loop; when USN access is denied,
  the UI states that incremental refresh is unavailable and offers an explicit
  Rebuild.
- **Find Duplicates** is planning evidence only. Same-size files and sample
  fingerprints are never shown as verified copies. Hard-link aliases of one
  identity are not independently reclaimable. There is no Delete / Deduplicate /
  Keep One control. Adding a group to Cleanup Review does not authorize
  deletion. See [`docs/DUPLICATES.md`](DUPLICATES.md).

### Mutation separation

Destructive capability must stay a **separately permissioned** surface — not an
ordinary CLI verb on `spacelens.exe`.

```text
spacelens              READ ONLY (agents/scripts)
spacelens-gui          human inspection + review + Recycle Bin V1
spacelens_maintenance  GUI-only Windows Recycle Bin adapter
```

Maintenance V1 may send eligible reviewed **files** to the Recycle Bin after a
fresh preflight, explicit human confirmation, and a final identity/safety
guard. It does not permanently delete, empty the Recycle Bin, recycle
directories, follow reparse points, or accept CLI/agent/AI invocation. See
[`docs/MAINTENANCE.md`](MAINTENANCE.md).

## Separated concepts (never collapse them)

| Concept | Meaning |
|---------|---------|
| **Classification** | What kind of storage something *looks like* (build artifact, user data, …) |
| **Classification confidence** | How strong the deterministic rule match is |
| **Reclaimability** | Whether the *kind* of data is typically regenerable (e.g. build outputs) |
| **Filesystem safety** | Whether the *location* is protected/sensitive/ordinary |
| **Activity evidence** | Age / inactivity signals from write times (and advisory access times) |
| **Candidate strength** | Combined review priority for a human — **not** permission to delete |

**Never** expose:

```text
safe_to_delete = true
```

**Never** model:

```text
old == safe to delete
```

A large, old user video must remain **Review Only / Unknown reclaimability**.
An old CMake `build/` directory may become a **Strong** reclaim *candidate* for
human review — still not an authorization to delete.

## LastAccessTime is advisory only

Windows `LastAccessTime` is **not** authoritative evidence that a file is unused:

- Access-time updates may be disabled or delayed by policy.
- Scanners and backup tools can update it without user intent.
- It must not alone produce a Strong reclaim recommendation.

SpaceLens may store LastAccessTime when practical. Default stale-storage analysis
uses **write / descendant-write activity** that the scanner can reason about
deterministically.

## Directory activity (not only the directory object timestamp)

Directory object timestamps are often misleading. Because SpaceLens already
observes descendants during a scan, directories carry an **activity summary**:

- newest descendant modification time
- oldest descendant modification time (when useful)
- counts/bytes of files modified within 30 / 90 / 180 / 365 days (relative to analysis time)

Aggregation remains a single bottom-up pass (same order as size aggregation).

## Protected-location policy

Deterministic, AI-independent, core-owned.

Rough classes:

| Class | Examples (Windows) | Mutation posture (future) |
|-------|--------------------|---------------------------|
| **Protected** | `Windows`, `Program Files`, recovery / system volumes, drive roots | Do not manage deletion |
| **Sensitive** | User profile root, `AppData` (nuanced), critical app config areas | Extra warnings; never casual delete |
| **Ordinary** | Typical project folders, downloads content under user trees | Eligible for review workflows |
| **Unknown** | Unrecognized layouts | Conservative treatment |

`AppData` is **not** bulk-marked deletable or fully protected; it is application
data and needs nuanced treatment. This milestone uses the policy for
**warnings and filtering only** — no override checkbox for protection.

## Reparse points

The scanner does **not** follow directory reparse points (junctions, directory
symlinks, mount points) by default.

**Future destructive actions must never blindly traverse reparse points.**
Cleanup review should flag reparse-point entries when they appear.

## Stale snapshots and TOCTOU

A scan or index is a snapshot at time T1. The filesystem at T2 may differ.

Cleanup Review V2 already performs **explicit, metadata-only revalidation**
when the user asks. It answers whether the reviewed object still looks like
the captured one; it does **not** authorize deletion.

Current checks:

- Does the path still exist?
- Is it still the same item type?
- Is the strong object identity the same (`FILE_ID_INFO`, with an explicit
  64-bit fallback that never compares as equivalent)?
- Is it now a reparse point?
- Is it protected by policy?
- Has direct size / write time / attributes changed?
- For directories: object identity and direct metadata can match while
  recursive evidence remains **not revalidated**.

It does **not** read file contents, recurse into directories, follow the
final-component reparse point, enable extra privileges, or relocate a missing
item by scanning a volume for a file ID. Cancellation discards partial
results. Missing, denied, and failed records stay until the user refreshes
evidence or removes them.

Maintenance V1 revalidates again immediately before each Recycle Bin attempt
(identity, kind, reparse, location, size/write/attributes). `Refresh Evidence`
only replaces the captured baseline; it is not permission to recycle.

## Recycle Bin vs permanent delete

Human-authorized Maintenance V1 prefers **Move to Recycle Bin** and does not
implement permanent delete. Permanent delete, if ever added, must be clearly
separate and harder. See [`docs/MAINTENANCE.md`](MAINTENANCE.md).

Moving data to the Recycle Bin does **not** free the same amount of space.
The UI reports logical sizes only:

```text
Selected logical size
Eligible logical size
Recycled logical size
```

SpaceLens does not claim “physical space freed” and does not empty the Recycle
Bin.

## Future move policy (documented only)

Same-volume vs cross-volume moves differ. A cross-volume move may become
copy → verify → delete source. The source must not be deleted until the
destination is created and validated. Use structured Win32 APIs — never naive
shell-command string construction.

## No shell-command execution from AI text

Forbidden pattern:

```text
LLM response → extract command string → system(command)
```

Paths are **data**, not shell syntax. Filesystem operations use C++/Win32 APIs
with structured arguments.

## Cleanup Review (this milestone)

Cleanup Review is a **durable planning queue**, not a deletion feature.
Details: [`docs/CLEANUP_REVIEW.md`](CLEANUP_REVIEW.md). Recycle Bin execution
is a separate confirmed GUI path: [`docs/MAINTENANCE.md`](MAINTENANCE.md).

Flow:

```text
discovery → add (persist captured evidence) → later reopen
         → explicit live revalidation → review changes/warnings
         → Cleanup Plan (text/JSON)
         → optional human-authorized Recycle Bin (Maintenance V1)
```

Available review actions today: revalidate all, cancel, refresh evidence,
open, reveal in Explorer, remove from review, clear review, copy plan,
export JSON, and **Move to Recycle Bin…**. **No permanent Delete. No arbitrary
Move.**

```text
Delete: NOT IMPLEMENTED
Move: NOT IMPLEMENTED
Recycle Bin: GUI only — see docs/MAINTENANCE.md
```

## Reclaim / stale storage candidates

A reclaim candidate combines:

```text
storage size
  + inactivity/age evidence (write-based)
  + deterministic classification
  + filesystem safety policy
```

Example (strong candidate for review):

```text
build/  4.8 GB
newest descendant write: 210 days ago
classification: Build Artifact (High)
reclaimability: Likely Regenerable
safety: Ordinary
candidate strength: Strong
```

Example (must not be deletion-shaped):

```text
old-video.mp4  18 GB
last modified: 800 days ago
classification: User Data
reclaimability: Unknown
candidate strength: Review Only
```

A future AI layer may *explain* candidates; it must never override deterministic
safety or reclaimability classifications.
