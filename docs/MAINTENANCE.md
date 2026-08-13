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
| Location | Current `classifyLocation` must be `Ordinary`. Protected, Sensitive, and Unknown are blocked. |
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

Local non-user layouts such as `D:\Hoc\...` or `D:\proj\...` remain
**Unknown** and are blocked. Maintenance V1 does not expand Ordinary to every
drive-letter path.

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
