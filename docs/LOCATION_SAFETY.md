# Location Safety V2 — User-Declared Ordinary Roots

Location safety answers only:

> What kind of place is this path?

It never answers:

> Is this safe to delete, recycle, reclaim, or trust?

```text
built-in classifyLocation(path)
        ↓
effective OrdinaryLocationPolicy
        ↓
Protected / Sensitive / Ordinary / Unknown
```

A human may declare a specific root ordinary — for example a project tree on a
data volume that the built-in policy leaves `Unknown`. That declaration changes
**location classification only**.

## What a declaration is not

Declaring a location ordinary does **not** mean:

- safe to delete
- safe to recycle
- reclaimable
- trusted content
- approved cleanup

There is no `safe_to_delete` field.

## Built-in policy is unchanged

`classifyLocation` stays path-only. Drive-letter paths are not automatically
Ordinary. Typical built-in classes:

| Class | Examples |
|-------|----------|
| **Protected** | Drive roots, `Windows`, Program Files, ProgramData, Recovery, System Volume Information, `$Recycle.Bin` |
| **Sensitive** | User-profile root, any AppData path |
| **Ordinary** | Typical folders under `C:\Users\<name>\` that are not AppData |
| **Unknown** | Unrecognized layouts, including many user-managed trees on other volumes |

`C:\Windows` remains Protected even if a user declares a sibling volume ordinary.

## Effective policy precedence

Effective safety is a snapshot (`OrdinaryLocationPolicy`) evaluated in this
locked order:

1. Built-in Protected → Protected (`BuiltInProtected`)
2. Built-in Sensitive → Sensitive (`BuiltInSensitive`)
3. Built-in Ordinary → Ordinary (`BuiltInOrdinary`)
4. Active matching user declaration → Ordinary (`UserDeclaredOrdinary`)
5. Else Unknown

Declarations cannot override Protected or Sensitive. An empty policy preserves
built-in-only behavior.

Matching is component-aware (`normalizeCleanupPath` / `isPathAncestorOrEqual`).
`D:\proj` matches `D:\proj\app` and does not match `D:\project`. Long-path
prefixes (`\\?\`, `\\?\UNC\`) are stripped before comparison. Parent and child
declarations may both exist; provenance uses the most specific Active match.

## Volume binding

Drive letters are not identity. Each declaration captures volume serial
(`resolveVolumeIdentity`) and optional volume GUID
(`GetVolumeNameForVolumeMountPoint`).

| Status | Meaning | Authorizes Ordinary? |
|--------|---------|----------------------|
| Active | Path present, not a reparse, serial/GUID match | Yes, subject to built-in rules |
| VolumeUnavailable | Serial is 0 or cannot be read | No (persisted, not authorizing) |
| VolumeMismatch | Serial or GUID changed | No |
| PathUnavailable | Missing or unreadable | No |
| Invalid | Reparse, not a directory, or probe error | No |

Refresh recomputes status from live evidence. Captured path and volume identity
are preserved. Index rebuild, delete, and USN refresh do not erase declarations.

## What may be declared

Rejected at add time:

- empty, `.`, `..`, relative, or malformed paths
- any path whose components include `.` or `..` (not resolved or followed)
- Win32 device-namespace paths (`\\.\`)
- whole-volume roots (`C:\`, `D:\`)
- built-in Protected or Sensitive roots
- reparse roots (junctions / directory symlinks / mount points)
- missing or non-directory paths

A declaration root is probed with no-follow semantics. Descendant reparse
targets do not inherit Ordinary as a special case: classification remains
path-based, so `C:\Windows` stays Protected.

## Persistence

Declarations live in the same SpaceLens-owned database as Cleanup Review:

```text
%LOCALAPPDATA%\SpaceLens\state.db
```

| Marker | Value |
|--------|-------|
| `review_schema_version` | 1 (unchanged) |
| `location_schema_version` | 1 |
| `location_declaration_generation` | increment on add/remove |

Table: `ordinary_location_declarations`. Extra location tables are additive
once the review schema exists. Opening a second store first on an empty file
is not used; location schema is installed by `CleanupReviewStore`.

Add and remove are blocked while review/maintenance mutations are blocked.

## GUI only

Humans add and remove declarations from **Safety → User-declared ordinary
locations…**. Copy states that this marks where ordinary user-managed files
are expected and does **not** mark files as safe to remove. Buttons never say
Delete.

The CLI has no `trust`, `allow-root`, `ordinary-root`, or `safety --write`
verb. Declaration writes are SpaceLens-owned state, not analyzed-filesystem
mutation. `filesystem_mutation` remains `false`.

## Maintenance

Maintenance V1 still requires every existing gate. A declaration only satisfies
the **location-safety** part, and only while it is Active and still the most
specific covering root.

The GUI refreshes declarations on the GUI thread at prepare and again at
execute. The final guard re-classifies with that refreshed policy immediately
before recycle. Removing the covering declaration, or a volume mismatch,
returns the path to Unknown and blocks the recycle. Generation change alone
does not block if another Active declaration still covers the path.

Captured safety on a review item stays historical. Revalidation and maintenance
use current effective policy.

See [`docs/MAINTENANCE.md`](MAINTENANCE.md) and [`docs/SAFETY.md`](SAFETY.md).
