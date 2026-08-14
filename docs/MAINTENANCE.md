# Human-Authorized Maintenance V1 — Recycle Bin Only

Maintenance V1 is the first SpaceLens path that may mutate analyzed filesystem
contents. The mutation is deliberately narrow:

```text
Cleanup Review
    ↓
fresh maintenance preflight
    ↓
human confirmation in the GUI
    ↓
final identity/safety guard
    ↓
Windows Recycle Bin
    ↓
per-item result receipt
```

Nothing else is authorized.

## What it can do

- A human may select reviewed **files** in the Cleanup Review dialog.
- SpaceLens re-probes those files, then shows **Move to Recycle Bin**.
- After explicit confirmation, eligible files are sent to the Windows Recycle
  Bin one at a time through `IFileOperation`.
- Each attempt is recorded as a receipt in `%LOCALAPPDATA%\SpaceLens\state.db`.
- Recycled review rows become `lifecycle = Recycled` and cannot be recycled
  again.

## What it cannot do

- Permanent deletion (`DeleteFileW`, recursive delete, empty Recycle Bin)
- Restore from the Recycle Bin
- Recycle directories (V1)
- Follow reparse points
- Relocate a missing item by file ID
- Move, rename, or replace files
- Duplicate auto-cleanup / keep-one / delete-others
- Scheduled, automatic, background, or AI-triggered maintenance
- Administrator auto-elevation
- CLI or MCP mutation (`spacelens recycle`, `spacelens maintenance`,
  `spacelens delete`, `--delete`, `--keep-one`, `--dedupe`,
  `spacelens cleanup --execute`)

The CLI capability contract remains:

```text
filesystem_mutation: false
```

Duplicate Detection still has no Delete / Keep One / Deduplicate controls.

## Eligibility

A file is eligible only when every gate passes:

| Gate | Rule |
|------|------|
| Type | Live kind is `File`. Directories and reparse directories are blocked. |
| Location | Current **effective** location safety must be `Ordinary`. Built-in Protected/Sensitive always win. An Active user-declared ordinary root may satisfy this gate for an otherwise Unknown path. Unknown remains blocked. |
| Identity | Strong `FileId128` with a non-zero volume serial is required. Path equality is not enough. |
| Match | Live identity must equal the captured review identity. |
| Metadata | Live size, last-write, and attributes must match the planned values. |
| Presence | Missing, access-denied, and probe errors are blocked. |
| Reparse | `FILE_ATTRIBUTE_REPARSE_POINT` or a reparse probe is blocked. |
| Lifecycle | Already-recycled rows are blocked. |
| Aliases | Hard-link aliases of an already-selected strong identity are blocked (`SameIdentityAlreadySelected`). |
| Recycle Bin | Local drive-letter path, `GetDriveType` FIXED/REMOVABLE, and `SHQueryRecycleBin` success. Applied during preflight via `applyRecycleAvailability` so confirmation never presents a Recycle-unavailable path as eligible. The adapter repeats the check immediately before `IFileOperation`. |

Classification, reclaimability, and candidate strength are **ignored**. A Strong
reclaim candidate is not permission to recycle.

### Ordinary is not “any path on a data drive”

`SafetyPolicy` treats drive roots, Windows/Program Files/ProgramData/Recovery,
System Volume Information, and `$Recycle.Bin` as Protected. A user-profile root
and any AppData path are Sensitive. Typical project paths under
`C:\Users\<name>\Projects\...` are Ordinary.

Local non-user layouts such as a project tree on a data volume remain
**Unknown** until a human declares that specific root ordinary. Maintenance
does not expand Ordinary to every drive-letter path. A declaration changes
classification only; every other V1 gate still applies, and the final guard
re-classifies with a refreshed policy immediately before recycle. See
[`docs/LOCATION_SAFETY.md`](LOCATION_SAFETY.md).

`%TEMP%` is usually under AppData, so it is Sensitive. Adapter tests may recycle
a generated TEMP file **directly** to prove the Shell contract; the product
eligibility gate still blocks Sensitive locations.

## Recycle-only Shell contract

The adapter lives in `spacelens_maintenance` and is linked only to the GUI and
adapter tests. The CLI links `spacelens_core` only.

```text
IFileOperation::DeleteItem
SetOperationFlags(
    FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD |
    FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE)
```

SpaceLens does **not** set `FOFX_NOSKIPJUNCTIONS`. It does not use
`SHFileOperation`, `DeleteFileW`, or `ITransferSource::RemoveItem`.

Official documentation does not guarantee that recycle never falls back to
permanent delete. `IFileOperationProgressSink::PostDeleteItem` is the evidence
gate:

- `psiNewlyCreated` is the Recycle Bin item
- or **NULL** if the item was fully/permanently deleted

Success requires all of:

1. `SUCCEEDED(PerformOperations)`
2. `GetAnyOperationsAborted` is false
3. `SUCCEEDED(hrDelete)`
4. `psiNewlyCreated != NULL`
5. the source path is gone

If the source path is gone without Recycle Bin item evidence, the result is
`UnexpectedPermanentRemoval`. Remaining eligible files are not attempted.

The worker that owns `IFileOperation` calls `CoInitializeEx(COINIT_APARTMENTTHREADED)`.

## Human confirmation

The confirmation dialog says **Move to Recycle Bin**, never Delete. It reports
selected / eligible / blocked counts and **logical** sizes, and states:

> The Recycle Bin still occupies storage. This is not permanent deletion.
> SpaceLens will not empty the Recycle Bin.

Cancel stops before the next file. Partial receipts are persisted.

## Receipts and schema

`review_schema_version` stays **1**. Maintenance adds additive tables:

| Table | Role |
|-------|------|
| `maintenance_operations` | One row per confirmed (or attempted) operation |
| `maintenance_receipt_items` | Per-file result, HRESULT, Recycle Bin parsing name, identity |

`review_items.lifecycle` is added with `DEFAULT 'Active'` when missing. Extra
tables are allowed next to the existing review schema. Recycled lifecycle is
also reconstructed from receipts on load.

Wording is logical size only:

```text
Selected logical size
Eligible logical size
Recycled logical size
```

SpaceLens does not claim “physical space freed.” Recycle Bin contents still
occupy storage.

## Library isolation

```text
spacelens_core          plan, eligibility, receipts (no Shell recycle)
spacelens_maintenance   WindowsRecycleAdapter (GUI + adapter tests)
spacelens.exe           core only, filesystem_mutation: false
spacelens-gui.exe       prepare → confirm → final guard → adapter → receipt
```

## Honest limits

- Recycle Bin items still use disk.
- Secondary-drive project layouts stay Unknown until SafetyPolicy is expanded.
- V1 will not recycle directories.
- Unexpected permanent removal is treated as a hard stop, not success.
- There is no `safe_to_delete` flag and no guaranteed reclaim.

# Human-Authorized Maintenance V2 — Hardened Recycle Workflow

V2 does **not** add a new class of filesystem mutation. It hardens the existing
GUI-only file-to-Recycle-Bin path: durable operation IDs, per-item checkpoints,
crash/restart reconciliation, a stale-plan guard, and an inspection-only
history UI.

V1 history above remains the mutation contract. V2 adds evidence and recovery
rules around that same Recycle Bin call.

## What V2 adds

- A durable `operationId` for every confirmed execute.
- `maintenance_schema_version = 2` (additive). `review_schema_version` stays 1.
- Persist `Attempting` **before** `IFileOperation`. A crash during recycle is
  `Uncertain`, never guessed `Recycled` from a missing path.
- Small SQLite checkpoints. A write transaction is never held across a
  confirmation dialog, Shell call, or user input.
- Prepared-plan expiry (`kMaintenancePlanMaxAgeTicks` = 2 minutes). Time is an
  extra guard, not a substitute for identity, metadata, or the final guard.
- Confirmation gate: not prepared / already executing / stale plan.
- Failure policy A–E (below). Persist failure after a verified recycle is
  `Uncertain` and **must not** retry the destructive call.
- Maintenance History: inspection only. No Restore, Delete, Retry, or Empty.
- Confirmation CTA: **Move eligible files to Recycle Bin**.
- Completion summary: Recycled / Blocked / Failed / Cancelled / Uncertain plus
  recycled logical size. Never “space freed”.

## What V2 still cannot do

Everything V1 forbids, plus:

- Recycle Bin Restore
- Permanent deletion / empty Recycle Bin
- Directory recycling
- CLI or MCP mutation
- Automatic elevation
- Retry meaning “call Shell again because bookkeeping failed”

## Operation lifecycle

```text
prepare (fresh preflight + history UncertainPriorOutcome)
    ↓
human confirmation (CTA + required wording + 2-minute plan TTL)
    ↓
begin operation (status = Executing, durable operationId)
    ↓
per eligible item:
    final identity/safety/recycle-availability guard
    persist Attempting
    IFileOperation recycle
    persist Recycled / failure / Uncertain
    cancel applies before the next item
    ↓
complete operation (Completed / Cancelled / HardStopped / Uncertain)
```

On open, incomplete `Attempting` rows become `Uncertain`. Executing operations
are recounted from items. A missing original path is **not** treated as
Recycled.

## Failure policy

| Class | Meaning | Continue? |
|-------|---------|-----------|
| **A** | Safe pre-mutation failure (final guard, recycle unavailable, persist Attempting failed before Shell) | Continue or stop without calling recycle for that item |
| **B** | Verified recycle failure, source still present (`ShellError`, access denied) | May continue with the next item |
| **C** | Cancel | Stop before the next item; keep receipts |
| **D** | Unexpected permanent removal (source gone, no Recycle Bin item) | **Hard stop**. Remaining items `NotAttempted` |
| **E** | Persist failure after verified recycle | Mark `Uncertain`, stop, **do not recycle again** |

Idempotency: already-recycled review rows and strong identities with
`Attempting`/`Uncertain` history are blocked at prepare
(`AlreadyRecycled`, `UncertainPriorOutcome`). If Recycle Bin history
cannot be read, prepare fails closed instead of planning with empty
history.

## Schema (additive)

`review_schema_version` stays **1**. `maintenance_schema_version` is **2**.

V1 tables remain. V2 adds columns on `maintenance_operations`:

| Column | Role |
|--------|------|
| `status` | `Executing` / `Completed` / `Cancelled` / `HardStopped` / `Uncertain` |
| `uncertain` | Count of Uncertain items |
| `selected_count` / `eligible_count` | Confirmation snapshot |
| `selected_logical_bytes` / `eligible_logical_bytes` | Logical sizes only |

A stored maintenance version newer than 2 fails closed (`SchemaUnsupported`).
A valid database is never replaced to “fix” an unknown version.

## GUI wording

Confirmation must say:

```text
Files will be moved to the Windows Recycle Bin.
This is not permanent deletion.
Recycle Bin contents still occupy disk space.
SpaceLens will not empty the Recycle Bin.
```

Primary CTA: **Move eligible files to Recycle Bin**.

Completion lists Recycled / Blocked / Failed / Cancelled / Uncertain and
**Recycled logical size**, then:

```text
The Recycle Bin still occupies storage. This is not space freed.
```

History is evidence only.

## Tests and seams

Core execute accepts an optional journal, clock-injected timestamps, a scripted
recycle adapter, and an optional `canRecycle` probe. Blocked cases must not
call the adapter. No real-time sleeps.

## AI / CLI boundary

External agents may inspect read-only JSON. They may not authorize or trigger
maintenance, generate an executable cleanup action, or bypass GUI confirmation.
`filesystem_mutation: false` is unchanged. The CLI still does not link
`spacelens_maintenance`.
