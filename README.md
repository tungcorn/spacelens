<div align="center">

<img src="assets/banner.png" alt="SpaceLens" width="100%" />

# SpaceLens

**Fast native C++ storage intelligence for Windows, built for humans, scripts, and AI agents.**

</div>

SpaceLens scans Windows storage and converts filesystem snapshots into deterministic size, activity, classification, location-safety policy, and read-only reclaim insights. It allows humans and AI agents to analyze disk usage and plan cleanup opportunities safely without granting write or deletion permissions to an agent.

```text
spacelens_core         →  spacelens (CLI, read-only)
                       →  spacelens-mcp (stdio MCP, read-only)
                       →  SpaceLens (Qt desktop analyzer)
spacelens_maintenance  →  GUI Recycle Bin adapter only
```

## Quick Start

```powershell
npm install -g @tungcorn/spacelens
```

Representative commands:

```powershell
# Create or update persistent index snapshot for a volume/directory
spacelens index D:\ --json

# List active indexes, record counts, and snapshot freshness
spacelens index list --json

# Query high-level storage overview from snapshot index
spacelens overview D:\ --from-index --json

# Discover top storage review opportunities from snapshot index
spacelens opportunities D:\ --from-index --json

# Explain what kind of files consume the indexed root
spacelens breakdown D:\ --json

# Plan host-byte reclaim from physical evidence (planning only; never executes)
spacelens reclaim-plan D:\ --json
```

## Why SpaceLens?

- **Native C++20**: Native Windows filesystem scanner and indexed query engine.
- **Persistent SQLite Index**: Stores root snapshots locally in AppData for fast, repeated offline queries without rescanning.
- **Optional USN Incremental Refresh**: Fast index updates using NTFS Change Journal (USN) when volume permissions allow.
- **Exact Indexed Top-N & Overlap-Aware Aggregates**: Exact Top-K rankings and non-overlapping logical size metrics calculated on published snapshot evidence.
- **Deterministic Storage Classification**: Classifies files and folders (e.g., BuildArtifact, PackageCache, Media, Logs) with transparent confidence scoring.
- **SHA-256 Verified Duplicate Detection**: Identifies exact file content copies with identity collapse for hard links and persistent hash caching.
- **Read-Only CLI & Native MCP Server**: Built-in safe interfaces for command lines, scripts, and Model Context Protocol (MCP) clients.

## Workflow

```text
index list  ──>  overview  ──>  opportunities  ──>  breakdown  ──>  reclaim-plan  ──>  query  ──>  duplicates
```

- **`index list`**: Catalog available indexes, status, and snapshot freshness.
- **`overview`**: Review high-level indexed storage usage and largest objects.
- **`opportunities`**: Rank the top-N storage review candidates across the indexed root.
- **`breakdown`**: Explain what kind of indexed files consume the root (classification, extension, last-write age).
- **`reclaim-plan`**: Rank overlap-free host-byte reclaim evidence (actionable vs review-only). Planning only; never executes cleanup.
- **`query`**: Filter specific files or directories by size, age, pattern, or classification.
- **`duplicates`**: Locate identical content files verified by SHA-256 hashing.

## AI Agents / MCP

- **No embedded LLM**: SpaceLens is a deterministic engine, not an AI model.
- **Deterministic evidence**: Generates structured JSON reports containing exact size, age, location safety, and classification facts.
- **External AI reasoning**: Autonomous agents (e.g., Claude, Cursor, custom scripts) read SpaceLens evidence to make informed recommendations.
- **Strictly read-only**: CLI and MCP interfaces are strictly read-only toward analyzed user files.

For integration details, see [`docs/AGENT_INTERFACE.md`](docs/AGENT_INTERFACE.md) and [`docs/MCP.md`](docs/MCP.md).

## Safety

SpaceLens enforces a strict read-only safety contract for agent and CLI operations:

```text
CLI:  {"filesystem_mutation": false, "read_only": true}
MCP:  {"filesystem_mutation": false, "read_only": true}
```

- **No user file mutation**: CLI and MCP do not mutate analyzed user files; SpaceLens may write its own indexes and derived state under AppData.
- **GUI-only maintenance**: Human-authorized Recycle Bin cleanup is restricted to the desktop GUI requiring explicit user confirmation.
- **AI recommendation is not permission**: Structured outputs provide review candidates; agents have no filesystem permission over analyzed user files.
- **Snapshot evidence**: Indexed queries reflect snapshot evidence—not live filesystem state or guaranteed freed disk space.

For full safety specifications, see [`docs/SAFETY.md`](docs/SAFETY.md).

## Install

### Global Package (npm)

```powershell
npm install -g @tungcorn/spacelens
```

*Installs native binaries (GUI, CLI, MCP) packaged with Qt 6.8.3 runtime. Requires Node.js for installation and Microsoft Visual C++ Redistributable (x64).*

### Pre-built GitHub Releases

Download pre-built archives from the [latest SpaceLens release](https://github.com/tungcorn/spacelens/releases/latest):

| Asset | Description |
| --- | --- |
| Published unified archive (`spacelens-v*-windows-x64.zip`) | Unified profile: Desktop GUI + Read-only CLI + Read-only MCP (Qt 6.8.3 runtime included) |
| Published headless archive (`spacelens-cli-v*-windows-x64.zip`) | Headless profile: Read-only CLI + Read-only MCP (no GUI, no Qt dependency) |

*Requires Microsoft Visual C++ Redistributable (x64).*

## Build from Source

Prerequisites: Windows 10/11 x64, MSVC (VS 2022+), CMake 3.21+, Ninja. Qt 6.8.3 is required only for the GUI target.

```powershell
. .\scripts\dev-env.ps1
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

For CLI-only headless builds and developer presets, refer to [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`docs/CLI.md`](docs/CLI.md).

## Documentation

| Document | Description |
| --- | --- |
| [CLI](docs/CLI.md) | Command-line interface usage, flags, and JSON schemas |
| [Agent Interface](docs/AGENT_INTERFACE.md) | Guidance for integrating SpaceLens with AI agents |
| [MCP](docs/MCP.md) | Model Context Protocol server usage and tool declarations |
| [Index](docs/INDEX.md) | SQLite persistent index architecture, USN refresh, and SQL schemas |
| [Duplicates](docs/DUPLICATES.md) | Content hashing, identity collapse, and persistent hash cache |
| [Safety](docs/SAFETY.md) | Read-only security contract, classification rules, and location safety |
| [GUI](docs/GUI.md) | Desktop application features, navigation, and visual treemap |
| [Cleanup Review](docs/CLEANUP_REVIEW.md) | Staging area design, metadata revalidation, and cleanup plans |
| [Maintenance](docs/MAINTENANCE.md) | Human-authorized GUI Recycle Bin adapter and safety guards |
| [Architecture](docs/ARCHITECTURE.md) | High-level component design, layer boundaries, and threading model |
| [Performance](docs/PERFORMANCE.md) | Benchmark baselines, disk scan speeds, and memory metrics |
| [Contributing](CONTRIBUTING.md) | Developer setup, build presets, testing, and contribution guidelines |

## License

SpaceLens core code is licensed under the [MIT License](LICENSE).

The optional desktop GUI dynamically links against Qt 6.8.3, licensed under LGPL-3.0-only. See [`docs/QT_SOURCE_OFFER.md`](docs/QT_SOURCE_OFFER.md), [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md), and [`docs/QT_REDIST_REVIEWED.md`](docs/QT_REDIST_REVIEWED.md) for details.
