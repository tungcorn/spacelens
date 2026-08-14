# SpaceLens Read-Only MCP Adapter

`spacelens-mcp.exe` is a native stdio [Model Context Protocol](https://modelcontextprotocol.io)
server. It exposes the same storage intelligence as the CLI — overview,
opportunities, indexed query, verified duplicates, and index status — as
typed tools for an external AI harness.

```text
                 spacelens_core
                /      |       \
               /       |        \
      spacelens     spacelens-mcp    SpaceLens (GUI)
      CLI           stdio MCP        human inspection
      scripts       AI harnesses     + Recycle Bin
```

> **AI recommendation is not filesystem permission.**

The adapter is analysis-only. It does not scan by shelling out to
`spacelens.exe`, does not link `spacelens_maintenance`, and does not
require Qt.

Product version is **0.1.3**. The MCP binary is built from source
(`SPACELENS_BUILD_MCP`, default ON) and is installed into both
published v0.1.3 zip archives and the npm package
(`spacelens-mcp`). v0.1.2 packages remain MCP-free and immutable.

## What a harness can ask

| Question | Tool |
| --- | --- |
| What can this server do, and is it read-only? | `storage_capabilities` |
| What is consuming this path? | `storage_overview` |
| What are the highest-value review opportunities? | `storage_opportunities` |
| What is inside a large candidate? | `storage_query` |
| Where are verified duplicates? | `storage_duplicates` |
| Is an index available and how old is it? | `storage_index_status` |

Recommended flow:

```text
storage_capabilities
        ↓
storage_index_status     (optional: reuse a snapshot?)
        ↓
storage_overview         (live_scan or persistent_index)
        ↓
storage_opportunities
        ↓
storage_query            (index-only drill-down, object_type required)
```

Do not enumerate the whole filesystem from the model. Do not invent CLI
syntax. Bounded JSON is the contract.

## Tools

All six tools set `annotations.readOnlyHint=true` and
`destructiveHint=false`. Domain payloads keep `read_only: true` and
`filesystem_mutation: false` where the shared analysis JSON already
does. There is no `safe_to_delete` and no `recommended_delete`.

`readOnlyHint` describes the analyzed environment, not process I/O.
`storage_duplicates` may write a derived SHA-256 cache under
`%LOCALAPPDATA%\SpaceLens\hash-cache.db`. It never writes into the
scanned root. That cache is an accelerator; a false hit is a defect
and insufficient evidence hashes again. See
[`docs/DUPLICATES.md`](DUPLICATES.md).

### `storage_capabilities`

No arguments. Reports `interface: mcp`, `transport: stdio`, supported
protocol versions, the six tool names, and `filesystem_mutation: false`.

### `storage_overview`

| Argument | Required | Notes |
| --- | --- | --- |
| `path` | yes | Absolute directory |
| `source` | no | `live_scan` (default) or `persistent_index` |
| `limit` | no | 1–100, default 10 |

Same semantics as `spacelens overview --json`. Indexed source never
auto-refreshes. Missing index is a domain error (`isError: true`), not a
silent live fallback.

### `storage_opportunities`

| Argument | Required | Notes |
| --- | --- | --- |
| `path` | yes | Absolute directory |
| `source` | no | `live_scan` (default) or `persistent_index` |
| `limit` | no | 1–100, default 20 |
| `min_size_bytes` | no | Default 1 MiB |
| `older_than_days` | no | Default 90 |
| `classification` | no | Existing SpaceLens class name (same as CLI `--classification`). Unrecognized names match nothing. |
| `under` | no | Subtree prefix (same as CLI `--under`) |

Same inclusion and ranking as CLI `opportunities` (`ranking_policy`:
`opportunity_rank_v2`). Indexed `source=persistent_index` retrieves the
exact top-N across the whole index; filters apply before `limit`.
`unique_review_estimated` is aggregate-only (uint64 overflow) and does
not mean top-N is approximate. Indexed aggregates stream the full
matching set with bounded memory; there is no 50,000-row estimate
ceiling. Read `summary.unique_review_bytes` — it is exact logical review
bytes on published index evidence, not guaranteed reclaim, and not
authorization to delete. Nested selected directories are `overlapped`
and carry `nested_overlap`. Group totals use the same global overlap
(they are not independently additive across categories). Items include
compact `evidence.ecosystem` / `evidence.marker`. This tool never hashes
files; use `storage_duplicates` when duplicate evidence is wanted.

### `storage_query`

Index-only. Never live-scans. Never refreshes.

| Argument | Required | Notes |
| --- | --- | --- |
| `path` | yes | Indexed root |
| `object_type` | yes | `file` or `directory` |
| `under` | no | Subtree prefix (slash-normalized, LIKE-escaped) |
| `classification` | no | Existing SpaceLens class name |
| `reclaimability` | no | |
| `candidate_strength` | no | Strong / Moderate / ReviewOnly / None |
| `min_size_bytes` | no | |
| `older_than_days` | no | |
| `sort` | no | `size`, `name`, `last_write`, `classification`, `candidate_strength` |
| `limit` | no | 1–200, default 20 |
| `source` | no | Only `persistent_index` is accepted |

`source=live_scan` is rejected. Missing index is a domain error.

### `storage_duplicates`

Index-backed, hash-verified, hardlink-aware. Hard-link aliases of one
identity contribute 0 redundant bytes. Field:
`summary.potential_redundant_logical_bytes`. Additive cache telemetry
(`cache_hits`, `cache_misses`, …) does not change group order.

| Argument | Required | Notes |
| --- | --- | --- |
| `path` | yes | Indexed root |
| `min_size_bytes` | no | Default matches CLI |

### `storage_index_status`

Probes a published index and incremental refresh *eligibility*. It never
calls `refreshIndex` and never rebuilds.

## What is not exposed

No tools for: scan, top, find, index list, index refresh, delete,
recycle, restore, move, rename, maintenance, execute, or shell.

Unknown names such as `index_refresh` or `storage_delete` return JSON-RPC
`-32602`.

## Protocol

Official MCP as of this adapter: **2026-07-28**, with dual-era
**2025-11-25** `initialize` so current Inspector and legacy clients both
work.

| Era | How the client starts | Subsequent requests |
| --- | --- | --- |
| Modern | `server/discover` | `_meta.io.modelcontextprotocol/protocolVersion` + `clientCapabilities` |
| Legacy | `initialize` + `notifications/initialized` | no `_meta` required |

Modern results include `resultType`. Discover advertises
`supportedVersions: ["2026-07-28","2025-11-25"]`, `capabilities.tools`,
and server info under `_meta.io.modelcontextprotocol/serverInfo`.
Resources, prompts, sampling, elicitation, Streamable HTTP, SSE, OAuth,
and Tasks are not implemented.

Transport is **stdio only**. One NDJSON object per line. stdout is
protocol only — no banner, no log, no debug. Diagnostics go to stderr.
Incoming messages larger than **1 MiB** are rejected with `-32700`.
stdin EOF exits 0. `ping` returns an empty result so clients can
liveness-check without listing tools.

Cancellation: `notifications/cancelled` with `params.requestId`. A
reader thread applies cancel immediately so a long `storage_overview`
scan can stop. A cancel that arrives before the matching request starts
is remembered and suppresses that result. A cancelled request produces
no further result for that id. Late cancel after completion is ignored.

Domain failures (missing index, inaccessible root, bad tool arguments)
use `result.isError: true` plus structured JSON. Protocol failures use
JSON-RPC errors (`-32700` parse, `-32602` unknown tool / bad params,
`-32022` unsupported version with `data.supported` + `data.requested`).

## Architecture

```text
spacelens-mcp  →  StorageTools  →  StorageAnalysis  →  spacelens_core
     │                                    ↑
     │                                    │
  Protocol (stdio)                 spacelens CLI
  JsonValue (isolated)
```

`StorageAnalysis` is the shared orchestration used by both CLI and MCP
(`analyzeOverview`, `analyzeOpportunities`, `analyzeDuplicates`,
`analyzeIndexStatus`, `indexQueryToJson`). The adapter must not spawn
`spacelens.exe`. Semantic fields (logical bytes, opportunity paths,
`unique_review_bytes`, query hits) stay aligned; the MCP envelope adds
`content` / `structuredContent` around the same domain JSON.

The MCP JSON parser lives in `src/mcp/JsonValue.*` and is not used by
core. Core still has a JSON **writer** only.

CMake target: `spacelens-mcp` → `build-*/mcp/spacelens-mcp.exe`.
Configure fails if the target ever links `spacelens_maintenance`.

## Running locally

Build (MCP is on by default, including CLI-only presets):

```powershell
. .\scripts\dev-env.ps1
cmake --preset windows-debug
cmake --build --preset windows-debug --target spacelens-mcp
```

Claude Code (do not commit a machine-specific `.mcp.json`):

```powershell
claude mcp add --transport stdio spacelens -- .\build-debug\mcp\spacelens-mcp.exe
```

Official Inspector (pin the version; CI must not depend on `npx` latest).
Inspector 2.2.0 defaults to the **legacy** era (`initialize`); that is why
the adapter still implements dual-era handshake. To force modern:

```powershell
# legacy (Inspector default) — initialize then tools/list
npx --yes @modelcontextprotocol/inspector@2.2.0 --cli `
  .\build-debug\mcp\spacelens-mcp.exe --method tools/list
```

A config file may set `"protocolEra": "modern"` and
`"command": "...\\spacelens-mcp.exe"`; do not commit a machine-specific
path.

Safety / wire / CLI-parity gates (temp fixtures only):

```powershell
.\scripts\verify-mcp-safety.ps1 -McpPath .\build-debug\mcp\spacelens-mcp.exe
.\scripts\verify-mcp-wire.ps1   -McpPath .\build-debug\mcp\spacelens-mcp.exe
.\scripts\verify-mcp-parity.ps1 -CliPath .\build-debug\cli\spacelens.exe `
                               -McpPath .\build-debug\mcp\spacelens-mcp.exe
```

## Limits and provenance

| Surface | Default | Max |
| --- | --- | --- |
| overview `limit` | 10 | 100 |
| opportunities `limit` | 20 | 100 |
| query `limit` | 20 | 200 |
| incoming MCP message | — | 1 MiB |

Every analysis document reports `source: live_scan` or
`source: persistent_index`. Indexed evidence is a snapshot, not live
filesystem truth. `--from-index` / `source=persistent_index` never
refreshes. Query/status/duplicates open a current-schema index
read-only (`PRAGMA query_only`). An older published schema may be
migrated in place under AppData — that is not an index refresh and
does not write analyzed user files.

## Safety

See [`SAFETY.md`](SAFETY.md). MCP is a third read-only consumer beside
the CLI. Human-authorized Recycle Bin remains GUI-only. Paths are data
passed to Win32 APIs, not interpolated into a shell. Tool annotations
are hints; the real boundary is “no mutation tools + no maintenance
link + validated limits.”
