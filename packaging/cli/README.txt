SpaceLens CLI (read-only)

This archive is the agent/script surface. It can scan, index, query, and
report. It cannot delete, recycle, restore, move, or declare locations.

  spacelens version
  spacelens capabilities --json
  spacelens help

`capabilities --json` must report filesystem_mutation: false.

This CLI does not include the Qt GUI or Recycle Bin maintenance.

Runtime: Windows x64. The official Microsoft Visual C++ Redistributable
(x64) is required if the machine does not already have a compatible
MSVC runtime. Binaries in the v0.1.0 prerelease are unsigned.

This archive does not include Qt.

Project license: none has been chosen unless a LICENSE file is present
in this folder. This package is not a public distribution grant. See
THIRD_PARTY_NOTICES.txt for SQLite 3.53.4.
