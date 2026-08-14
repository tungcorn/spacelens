# Duplicate Detection V2

Duplicate Detection finds **exact file-content copies** and presents them as
planning evidence. It does not delete, link, move, or replace anything.

V2 adds a **persistent SHA-256 cache** so repeated analysis of unchanged
files can reuse a previously verified digest. The cache is an accelerator,
never a source of truth.

```text
Persistent index
   ↓
Same-size regular-file candidates  (accelerator only)
   ↓
Live metadata / identity probe
   ↓
Hard-link collapse  (same VolumeSerial + FileId)
   ↓
Optional sample fingerprint  (narrowing only)
   ↓
Full SHA-256 of live contents
   (or a verified cache hit — see below)
   ↓
Verified groups  or  skipped / inconclusive
```

The index is never treated as proof. Same size is not a duplicate. A matching
sample fingerprint is not a duplicate. A verified group requires either:

- two or more independent file identities with the same full SHA-256 after a
  stable live read, or
- two or more paths that are the same file identity (hard-link aliases).

```text
same size                         ≠  duplicate
same sample fingerprint           ≠  verified duplicate
same full SHA-256 + stable read   =  verified duplicate content
same VolumeSerial + FileId        =  same file, not two copies
```

## What V1 detects

| In scope | Out of scope |
|----------|----------------|
| Exact regular-file content | Directories as duplicate objects |
| Hard-link aliases of one file | Fuzzy / similar files |
| Independent copies of the same bytes | Image perceptual hashing |
| Planning hand-off to Cleanup Review | Semantic or cloud duplicates |

Ignored by default:

- zero-length files (`size_bytes > 0` is required)
- directories
- reparse-point files (not followed, not hashed)
- inaccessible, missing, or stale indexed paths
- files below the minimum logical size (default **1 MiB**)

## Persistent hash cache

```text
FALSE CACHE MISS  = acceptable
FALSE CACHE HIT   = correctness defect
When evidence is insufficient: HASH AGAIN.
```

The cache lives in `%LOCALAPPDATA%\SpaceLens\hash-cache.db`, independent of
`state.db` and of per-root indexes. It is disposable derived state. Path is
**never** the cache key. The key is `(volume_serial, file_id_128)`.

A stored digest is reusable only when every evidence-v1 field matches the
live probe:

- `FILE_ID_128` identity (64-bit fallback is never cached)
- volume serial
- logical size
- `ChangeTime` (`FILE_BASIC_INFO`)
- per-file USN (`FSCTL_READ_FILE_USN_DATA`)
- `algorithm = sha256`, `evidence_version = 1`
- digest length 32

Volume journal ID is stored when available but **not required** for reuse
(querying the volume journal is often `AccessDenied` without elevation). If
both the row and the live probe have a journal ID and they differ, the row
is not reused.

Size + last-write + FileId alone is **not** sufficient. `SetFileTime` can
restore last-write after a content change; ChangeTime and FileUsn must still
diverge, forcing a rehash.

Must rehash when: the row is missing, USN/ChangeTime is unavailable, identity
is FileIndex64, any evidence field mismatches, the filesystem cannot supply
FileId128+USN (FAT and similar), or the cache file is missing/locked/corrupt.
Invalid rows (wrong digest length, unknown algorithm/version, incomplete
identity) are dropped and hashed again.

Never persisted: sample fingerprints, cancelled hashes, `ChangedDuringRead`,
or 64-bit fallback identities. A cache write failure does not fail the scan;
the fresh digest is still used.

Empty `hashCachePath` disables the cache (tests). Product callers
(`analyzeDuplicates`, GUI session) set `spaceLensHashCachePath()`. Tests
must not open the real AppData file.

`read_only` / `filesystem_mutation: false` mean analyzed user files are not
mutated. Writing a derived cache under SpaceLens-owned AppData is
implementation state, not a new mutation class.

Telemetry (additive, does not change group order): `cache_hits`,
`cache_misses`, `cache_invalidations`, `cache_writes`, `files_fully_hashed`,
`bytes_fully_hashed`, `bytes_reused_from_cache`.

## Pipeline

1. **Index query.** `queryDuplicateSizeCandidates` selects regular files
   (`kind = 0`) with `size_bytes > 0`, `size_bytes >= minimum`, and
   `is_reparse = 0`, then keeps only sizes that appear at least twice.
2. **Live probe.** `WindowsCleanupMetadataReader` opens each path with
   `FILE_READ_ATTRIBUTES` and `FILE_FLAG_OPEN_REPARSE_POINT` (no follow).
   Missing, access-denied, reparse, non-file, and size-changed paths are
   skipped. The index `file_id` column is a stale 64-bit hint and is **not**
   used for hard-link grouping.
3. **Identity collapse.** Paths that share a live `CleanupIdentity`
   (`FILE_ID_128` preferred; 64-bit fallback never equals 128-bit) are one
   content instance. Unavailable identity falls back to a unique path key so
   two unknown identities are never silently merged.
4. **Hard-link-only group.** One identity and two or more paths → group with
   `verification = same_file_identity`. No content hash.
5. **Sample fingerprint.** When logical size is greater than 192 KiB
   (`3 × 64 KiB`), hash little-endian size plus first / middle / last 64 KiB.
   Sample clusters of size 1 are discarded unless they are hard-link aliases.
6. **Full SHA-256 or cache hit.** Each remaining identity is probed for
   cache evidence. A reusable row supplies the digest without reading file
   bytes. Otherwise the identity is hashed once, from the first sorted path.
   A 1 MiB reusable buffer is used; the file is not mapped whole. The handle
   stays open for the read. Metadata, ChangeTime, FileUsn, and identity are
   re-probed on that handle afterwards, and the path is re-opened to catch
   replacement. Changed size, last-write, ChangeTime, FileUsn, or identity →
   skip, never verify, never persist.
7. **Publish.** Only clusters with two or more independent identities and a
   matching full hash become `verification = full_sha256` groups. Sample-only
   matches with different full hashes produce no group.

Cancellation keeps already-finalized groups, marks the result
`cancelled` / not completed, and does not emit the in-flight bucket as a
partial group.

## Hard links and redundant bytes

Different paths with the same volume serial and file ID are **aliases of one
file**, not independently reclaimable copies.

```text
redundant_logical_bytes =
    max(0, distinct_content_instances − 1) × logical_size
```

Examples:

| Paths | Identities | Redundant logical bytes |
|-------|------------|-------------------------|
| 2 copies of 4 GiB | 2 | 4 GiB |
| 3 copies of 4 GiB | 3 | 8 GiB |
| 2 hard-link aliases of 100 MiB | 1 | 0 |
| 2 aliases + 1 independent 100 MiB copy | 2 | 100 MiB |

The UI and reports label this **Potential redundant logical bytes**. It is
never “guaranteed space you can free” and never a claim about physical
clusters. Saturating arithmetic is used so overflow is marked instead of
wrapping.

## CLI

```text
spacelens duplicates <indexed-root> [--min-size S] [--json]
```

- Requires a published index for that root. Missing index → exit **6**.
- Default `--min-size` is `1MB` (binary MiB).
- `--delete`, `--dedupe`, `--keep-one`, and other mutation verbs are rejected.
- Cancel (Ctrl+C) → exit **5**, with any completed groups marked partial.
- stdout is the report; stderr is diagnostics. JSON includes
  `planning_only`, `read_only`, and `filesystem_mutation: false`.

Serialization reuses Cleanup Plan’s component-boundary `%USERPROFILE%`
redaction when that environment variable is known. Unredacted reports may
still contain sensitive paths; file contents are never exported.

## GUI

**Find Duplicates** on the Indexed tab opens a planning dialog for the selected
indexed root:

- minimum-size field (default 1 MB)
- progress (`N / M` candidates and bytes processed)
- deterministic group list and inspector (SHA-256, identities, aliases,
  potential redundant bytes)
- Show in Explorer / Copy Path(s) / Copy Group Details
- **Add to Cleanup Review** — planning only; does not authorize deletion

There is no Delete, Keep One / Delete Others, or Deduplicate control.

## Cleanup Review hand-off

Selected duplicate paths become review candidates with:

- `source = "duplicate_detection"`
- `reasonAdded` containing `sha256=`, `verification=`, and
  `independent_copies=`

Adding does not execute cleanup. Cleanup Review and Cleanup Plan still execute
nothing.

## Implementation map

| Piece | Location |
|-------|----------|
| Domain types, redundant-byte math, text/JSON | `src/core/Duplicates.*` |
| Size-bucket query | `src/core/index/IndexQuery.*` |
| Sequential verification | `src/core/DuplicateDetection.*` |
| Persistent SHA-256 cache | `src/core/HashCache.*` (`hash-cache.db`) |
| BCrypt SHA-256, no-follow content open | `src/platform/windows/FileContentHasher.*` |
| CLI | `spacelens duplicates` |
| GUI worker | `src/app/DuplicateDetectionSession.*` |
| Dialog | `src/ui/DuplicateFilesDialog.*` |

Core stays Qt-free. Widgets never run SQL or hash from paint/model code. One
worker, sequential I/O, queued GUI delivery.

## Safety

- `filesystem_mutation` remains `false`.
- Content open uses `FILE_READ_DATA | FILE_READ_ATTRIBUTES` plus
  `FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN`.
- Final-component reparse points are not followed for hashing.
- Missing items are not relocated by file ID / MFT scan.
- Tests use generated temporary fixtures only.
- Reports committed to the repository must not contain personal absolute paths.

```text
Delete: NOT IMPLEMENTED
Move: NOT IMPLEMENTED
Recycle Bin: NOT IMPLEMENTED
```
