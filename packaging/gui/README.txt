SpaceLens (complete Windows distribution)

This archive is the recommended SpaceLens install. It contains both
interfaces of the same product:

  spacelens-gui.exe   desktop analyzer
  spacelens.exe       read-only CLI for terminals, scripts, and agents

Cleanup Review and Recycle Bin maintenance stay human-authorized in the
GUI. The CLI in this folder cannot delete, recycle, restore, move, or
declare locations. `spacelens capabilities --json` reports
filesystem_mutation: false.

Do not also install the optional CLI-only archive if you already have
this complete package. Both expose the `spacelens` command.

The official Microsoft Visual C++ Redistributable (x64) is a runtime
prerequisite. Qt runtime libraries in this folder were deployed with
windeployqt --no-compiler-runtime --no-system-d3d-compiler
--no-system-dxc-compiler from the Qt 6.8.3 shared kit.

v0.1.1 prerelease binaries are unsigned.

SpaceLens-owned files in this archive are licensed under MIT.
See LICENSE. Qt remains under its own licenses (LGPL-3.0-only option
for this dynamic build). Qt is not MIT. Adding spacelens.exe to this
archive does not change Qt's license.

licenses\ holds:
  - verbatim GNU LGPL-3.0 and GPL-3.0 texts
  - the maintainer-controlled Qt corresponding-source identity
  - the written source offer

See THIRD_PARTY_NOTICES.txt.
