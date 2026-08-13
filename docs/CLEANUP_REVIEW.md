# Cleanup Review V2

Cleanup Review is a **durable, planning-only queue**. It answers:

> Is this still the same thing I reviewed earlier, and has anything important changed?

It never answers:

> Is this definitely safe to delete?

```text
Discovery
   ↓
Add candidate
   ↓
Persist captured evidence
   ↓
Close SpaceLens
   ↓
Open later
   ↓
Live revalidation
   ↓
Review changes/warnings
   ↓
Build Cleanup Plan
```

No Delete, Move, Rename, Recycle Bin, automatic cleanup, or agent filesystem
action is implemented. `filesystem_mutation` remains `false`. Duplicate
Detection may add verified paths with `source = "duplicate_detection"`;
that is still only a planning hand-off.

## What it stores

Review state lives in a SpaceLens-owned SQLite database, independent of every
replaceable per-root index:

```text
%LOCALAPPDATA%\SpaceLens\state.db
```

| Database | Schema marker | Purpose |
|----------|---------------|---------|
| `indexes/<rootKey>/index.db` | `index_schema_version = 2` | Replaceable snapshot of one scanned root |
| `state.db` | `review_schema_version = 1` | Durable review evidence and last validation |

Index rebuild, USN refresh, staging publish, and deleting an index **must not**
erase or silently rewrite review records. There is no foreign key from review
rows to index rows.

Startup **loads** review state. It does **not** revalidate automatically.

## Schema (logical)

| Table | Role |
|-------|------|
| `meta` | `review_schema_version`, `review_next_id` |
| `review_items` | Durable ID, original path, normalized path key, captured object evidence, historical directory aggregate, classification, reclaimability, strength, safety, source/root, index age/timestamp, `addedAt` |
| `review_validation` | Current observation, current object/aggregate evidence, primary state, reason flags, diffs, identity/direct/recursive honesty flags, checked time |

Unsupported newer or malformed schemas fail closed. A valid database is never
replaced to “fix” an unknown version.

## Captured vs current evidence

Every candidate keeps two layers of evidence:

| Layer | Meaning |
|-------|---------|
| **Captured** | What SpaceLens recorded when the item was added (or last refreshed) |
| **Current** | What the last explicit metadata probe observed |

Captured fields include:

- original path and normalized path key
- item type
- captured logical size (discovery/index size at selection)
- direct last-write and last-access times
- attributes
- classification, confidence, and rule ID
- reclaimability and candidate strength
- captured location safety
- source (`live_scan` or `persistent_index`) and source root
- source index timestamp and age, when the add came from an index
- added timestamp
- identity source, strength, and value

Current evidence is metadata-only. Revalidation never reads file contents,
never walks descendants, and never relocates a missing path by file ID.

### Directory honesty

A directory has two distinct facts:

```text
object identity matched
direct metadata unchanged
recursive evidence not revalidated
```

Those facts stay separate. A directory whose object identity and direct
metadata still match is **not** reported as unqualified `Unchanged`. The
primary state is:

```text
DirectUnchangedRecursiveNotRevalidated
```

Direct directory size/mtime is never compared as if it were recursive
aggregate evidence. Refresh Evidence may replace direct object metadata; it
does **not** invent a new recursive size from a handle.

## Object identity

Preferred Windows identity:

```text
GetFileInformationByHandleEx(handle, FileIdInfo, …)
VolumeSerialNumber + FILE_ID_128
```

Explicit weaker fallback:

```text
GetFileInformationByHandle
dwVolumeSerialNumber + 64-bit file index
```

Identity **source is part of equality**. `FILE_ID_128` and the 64-bit fallback
never compare as the same object, even when numeric fields look similar.
Unavailable identity is represented explicitly; zero is never a valid match.

Duplicate / conflict rules:

1. Matching strong `(volumeSerial, FILE_ID_128)` is the same review object.
2. When identity is unavailable, a normalized case-insensitive path + item
   type is a conservative fallback.
3. The same path with two different strong identities is never silently
   merged. Both records remain and the plan surfaces a conflict.
4. Fallback identity on different paths is not merged.

## Live revalidation

Revalidation is **explicit, cancellable, sequential, and metadata-only**.

The Windows reader opens with:

```text
FILE_READ_ATTRIBUTES
FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
OPEN_EXISTING
FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT
```

It does not follow the final-component reparse point, enable extra privileges,
require Administrator rights, or scan a volume by file ID.

| Probe outcome | Meaning |
|---------------|---------|
| `Present` | Handle opened and usable object metadata was read |
| `Missing` | Path is gone; the review record stays |
| `AccessDenied` | Present-or-unknown, but attributes could not be read |
| `ProbeError` | Opened or queried, but metadata is incomplete / unexpected |

A successful `CreateFileW` with no usable type/size/time metadata is
`AccessDenied` or `ProbeError`, never `Present` and never `Missing`.

Cancellation **discards partial results**. Only a completed pass is written,
and it is written as one transaction. Failed persistence leaves both memory
and disk on the previous valid state.

While Revalidate All is running, add / remove / clear / refresh are rejected.
The completed batch still applies. Each update carries the snapshot path from
the start of the pass; if that path no longer identifies the same review row,
the whole batch is rejected and validation stays `NotValidated`.

Adding a path discovered as an ordinary directory promotes it to
`ReparseDirectory` when the live metadata probe observes a reparse point. The
captured kind must match the object that was actually inspected.

`Refresh Evidence` replaces the captured baseline with successfully observed
current metadata. It never authorizes deletion, never clears protected /
sensitive / reparse warnings, and never invents directory recursive size.

## Cleanup Plan

`buildCleanupPlan` is a pure, deterministic, read-only transform.

| Total | Meaning |
|-------|---------|
| Raw selected logical bytes | Diagnostic sum of captured sizes |
| Unique selected logical bytes | Overlap-aware total after identity/path dedupe and ancestor suppression |

Overlap rules:

- A selected ordinary directory with historical recursive aggregate suppresses
  selected descendants.
- Files and reparse directories never cover descendants.
- A directory without an available aggregate does not cover children.
- Ancestor coverage stops at a reparse barrier.
- Saturating addition marks totals estimated when conflicts or incomplete
  directory evidence prevent certainty.

JSON exports include:

```json
{
  "plan_schema_version": 1,
  "planning_only": true,
  "read_only": true,
  "filesystem_mutation": false
}
```

Optional `%USERPROFILE%` redaction is applied **only at serialization**. Stored
paths are never rewritten. Redaction is component-bounded: a profile named
`C:\Users\Alex` does not match `C:\Users\Alexander`. Unredacted plans may
contain sensitive absolute paths and filenames; they never contain file
contents.

External agents may analyze an export. They receive no filesystem permission.

## GUI workflow

`MainWindow` owns `CleanupReviewController` and
`CleanupRevalidationSession`. Live Scan and the Indexed page both add into
that durable store.

Available actions:

- Revalidate All / Cancel
- Refresh Evidence
- Open / Show in Explorer
- Remove from Review / Clear Review
- Copy Plan / Export JSON
- optional `%USERPROFILE%` redaction

There are no Delete or Move controls. Open and Reveal are human-initiated
Explorer/default-app actions on a selected path.

## Transactional mutation

Every state change follows:

```text
draft review
    ↓
persist the whole operation
    ↓
swap in-memory state after commit
```

Add, remove, clear, refresh evidence, and validation batches are atomic.
Missing, changed, denied, and failed records remain until the user refreshes
evidence or removes them.

## Non-goals

- deletion, Recycle Bin, move, rename
- automatic cleanup or a maintenance executable
- AI-generated filesystem actions
- new USN/MFT work for review
- MCP
- reading file contents during revalidation
- following a reparse point to make identity comparison succeed
- scanning a volume by file ID to relocate missing items
- requiring Administrator rights for Cleanup Review

```text
Delete: NOT IMPLEMENTED
Move: NOT IMPLEMENTED
Recycle Bin: NOT IMPLEMENTED
```
