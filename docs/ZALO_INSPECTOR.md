# Zalo Storage Inspector V1

SpaceLens can inspect a supported Zalo PC storage tree without modifying the
Zalo installation or reconstructing conversations. The inspector answers the
bounded question **"What is this?"**: it reports truthful filesystem accounting,
validated content families, and limited document identity where the bytes prove
it locally.

## Commands

```text
spacelens app-storage zalo [--root PATH]... [--json]
spacelens app-storage zalo items [--root PATH]... [--largest N]
    [--type TYPE] [--unknown] [--min-size SIZE] [--compare PATH]... [--json]
```

Without `--root`, discovery checks only exact supported Zalo locations. A root
passed with `--root` is explicit user scope; it is still enumerated with bounded,
read-only rules and reparse directories are not followed. `--compare` is only
available on `items`, is repeatable, and authorizes inspection of those paths
only. Downloads, Documents, Desktop, browser stores, message databases, and
credential stores are not searched implicitly.

`--largest` limits the returned review items. `--type` accepts stable category
aliases such as `image`, `video`, `document`, `archive`, and the validated
content types (`jpeg`, `pdf`, `docx`, `xlsx`, `pptx`, `zip`). `--unknown` selects
items whose content remains unresolved. `--min-size` accepts the same binary
size syntax as the other storage commands, for example `100MB`.

## Read-only and privacy contract

The CLI and MCP remain:

```json
{
  "read_only": true,
  "filesystem_mutation": false
}
```

The inspector never deletes, moves, renames, truncates, rewrites, repairs,
extracts, decrypts in place, logs out accounts, changes Zalo configuration, or
writes previews beside source files. It does not parse or print message bodies,
contact names, group names, account identifiers, tokens, credentials, hashes,
or native Zalo paths. JSON uses report-local item IDs and stable root/account
aliases. Diagnostic text belongs on stderr; JSON stdout is data only.

SpaceLens may write its own bounded state under its normal application-data
location (for example an index or hash cache). That state is never written into
a Zalo root. V1 previews, when supported by the GUI, are session-only in-memory
values and do not create a disk cache.

## Accounting model

The report keeps namespace accounting separate from physical accounting:

- **Path-visible logical bytes**: the sum of sizes seen through all category and
  account paths.
- **Unique logical bytes**: one logical size per stable filesystem identity.
- **Unique allocated bytes**: one known NTFS allocation per stable identity;
  unknown allocation is not guessed from logical size.
- **Hard-link alias bytes**: the path-visible amount attributable to repeated
  views of the same physical identity.
- **Coverage and consistency**: incomplete traversal, unavailable identity, or
  a source that changes during inspection downgrades exact aggregates instead
  of producing a false precise total.

A filesystem identity is not an exact-copy claim. Hard-link aliases are one
physical object with multiple names. Separate files are exact copies only when
an explicit comparison scope is requested and full SHA-256 equality proves the
comparison. Same name, size, timestamp, or a sample fingerprint is candidate
evidence only.

Per-path release values are accounting evidence, not cleanup instructions. A
path is not independently releasable when the filesystem reports additional
hard links; an incomplete observation also fails closed. Open handles and
other filesystem behavior can delay release after any future user action.

## Content identification

Recognition is extension-independent and bounded. Supported validated families
include:

- JPEG images;
- classic PDF;
- ZIP;
- DOCX, XLSX, and PPTX packages;
- verified prefixed/wrapped JPEG, PDF, and OOXML payloads.

A magic byte occurrence alone is never enough. Structural checks validate the
relevant grammar, bounds, and required terminators. If multiple incompatible
candidates validate, the result is ambiguous. If no supported validator proves
a type, the result is `unknown`; the inspector does not label an object
"encrypted", "corrupt", or "recoverable" without evidence.

Each identified object carries an identification method, confidence, evidence
codes, and (for wrapped payloads) a validated offset and length. The source is
never rewritten or materialized to make a decoder happy.

## Bounded semantic metadata

PDF, DOCX, XLSX, and PPTX may receive typed semantic metadata after content
identification. Extraction is deliberately small and local:

- PDF page count and bounded Info title/author/creator values;
- DOCX core properties plus the first meaningful visible paragraphs/headings;
- XLSX shared strings and a bounded selection of visible worksheet text;
- PPTX bounded visible slide text.

Text is normalized and capped by both UTF-8 bytes and Unicode scalar count.
Control, bidi/format-control, path-like, URL-like, XML-like, and hash-like
values are rejected. Comments, notes, headers/footers, tracked-change authors,
macros, external relationships, embedded objects, and message databases are
not used to reconstruct context. A metadata token occurrence is not a record
mapping and is never presented as a conversation or original filename.

Semantic status is typed (`available`, `not-applicable`, `malformed`,
`limit-exceeded`, `cancelled`, `read-error`, or `changed`). A source that changes
while semantic extraction is running is not published as a stable recognized
item.

## Explicit exact-copy comparison

Comparison is opt-in and scope-limited:

```text
spacelens app-storage zalo items --root <zalo-root> \
    --compare <explicit-folder> --json
```

Only the explicitly named comparison folders are enumerated. Reparse points are
skipped, overlapping scopes are deduplicated and reported separately from
filesystem hard-link aliases, and full content equality is required. For a
validated wrapper, the comparison
is between the exact payload slice and the complete external file; the output
identifies the proof method without serializing the digest. Zalo source paths
remain aliases. A comparison result is evidence for review, not a deletion or
move recommendation.

## JSON envelope

JSON responses use `schema_version: 1` and include the command, provider,
state, live-scan source, read-only capability flags, discovery aliases,
account/summary aggregates, and bounded items. Item fields are report-local and
privacy-safe. Native paths, filesystem identities, raw account identifiers,
filenames that could expose private content, and hashes are intentionally
omitted.

Typical no-root output is a successful JSON serialization with an inaccessible
scan state such as `NoRoots`; the process exit code communicates whether the
request completed, was cancelled, or could not access a supported root. A
partial result is still explicit about its state and coverage rather than being
reported as complete.

## Testing and fixtures

Tests generate temporary extensionless files, wrapped payloads, malformed
structures, hard-link aliases, unknown bytes, cancellation cases, and explicit
comparison trees. They do not read a developer's real Zalo data. Source fixture
snapshots cover directory entries, bytes, identities, sizes, allocation,
attributes, and timestamps before and after inspection. Production code has no
mutation API for Zalo data.

The implementation keeps parser and decompression budgets bounded. Large or
adversarial inputs therefore produce a typed unknown, limit, read, or partial
state instead of unbounded allocation or a claim that the data is encrypted.
