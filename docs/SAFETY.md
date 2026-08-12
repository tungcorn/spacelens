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
| Humans | `spacelens-gui.exe` | Inspection + cleanup **review** now; future mutations only behind explicit human authorization |

The agent-facing CLI must remain safe to grant to a coding agent without
implicitly granting file-deletion or file-movement capability.

## CLI: read-only by design

The primary executable:

```text
build/cli/spacelens.exe
```

exposes deterministic **scan / query / classify / report / index** operations only.

It must **not** register commands such as:

```text
delete  remove  rm  move  cleanup execute  purge  wipe
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

### GUI Index Browser (V2)

The Indexed tab is discovery and planning only:

- No Delete / Move / Rename controls.
- Cleanup Review remains an in-memory planning queue (`source=persistent_index`
  preserves snapshot age and classification metadata).
- Open / Reveal / default-app launch are human-initiated shell actions on paths
  the user selected; missing paths surface a stale-snapshot message rather than
  rewriting the index.
- Incremental refresh does not self-elevate in a loop; when USN access is denied,
  the UI states that incremental refresh is unavailable and offers an explicit
  Rebuild.

### Future mutation separation

Destructive capability, if ever implemented, must be a **separately permissioned**
surface — not an ordinary CLI verb on `spacelens.exe`.

Possible future shape:

```text
spacelens              READ ONLY (agents/scripts)
spacelens-gui          human inspection + review
spacelens-maintenance  optional, separately permissioned mutations
                       (or a GUI-only mutation service)
```

This milestone implements **safety abstractions and cleanup-planning UX only**.
No real deletion or movement.

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

A scan is a snapshot at time T1. The filesystem at T2 may differ.

Future mutation execution (not implemented now) must **revalidate** before acting:

- Does the path still exist?
- Is it still the same item type?
- Is it now a reparse point?
- Is it protected by policy?
- Has size / write time changed materially?
- Is the target still under the expected parent?

`CleanupCandidate` stores enough metadata (path, kind, size at selection,
write time, attributes, classification) so revalidation remains possible.

## Future deletion policy (documented only)

Default human deletion should prefer **Move to Recycle Bin** over permanent
delete. Permanent delete, if ever added, must be clearly separate and harder.

Moving large data to Recycle Bin does **not** automatically free the same amount
of space. Future UI should distinguish:

```text
Selected logical size
Moved to Recycle Bin
Actually reclaimed space  (when measurable)
```

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

Cleanup Review is a **planning queue**, not a deletion feature.

Flow:

```text
discovery → selection → review → (future) explicit human-authorized action
```

Available review actions today: open, reveal in Explorer, remove from review,
clear review, copy report. **No Delete. No Move.**

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
