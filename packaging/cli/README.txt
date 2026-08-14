SpaceLens headless profile (CLI + MCP)

This archive is the minimal/headless SpaceLens profile. It is not
required if you already installed the complete SpaceLens archive.

It contains:

  spacelens.exe       read-only CLI
  spacelens-mcp.exe   read-only stdio MCP server

It can scan, index, query, report, and serve MCP tools. It cannot
delete, recycle, restore, move, or declare locations.

  spacelens version
  spacelens capabilities --json
  spacelens help
  spacelens-mcp.exe     (stdio only — pair with an MCP client)

`capabilities --json` must report filesystem_mutation: false.
MCP tools are analysis-only. AI recommendation is not filesystem
permission.

This archive does not include the Qt GUI or Recycle Bin maintenance.
Do not install this package at the same time as the complete
SpaceLens archive; both expose the `spacelens` command.

In v0.1.2 this filename was CLI-only. From v0.1.3 it is the headless
agent profile (CLI + MCP). The filename is kept for continuity.

Runtime: Windows x64. The official Microsoft Visual C++ Redistributable
(x64) is required if the machine does not already have a compatible
MSVC runtime. Binaries in v0.1.3 are unsigned.

This archive does not include Qt.

SpaceLens-owned files in this archive are licensed under MIT.
See LICENSE. SQLite is independent; see THIRD_PARTY_NOTICES.txt.
