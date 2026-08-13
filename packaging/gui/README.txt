SpaceLens GUI

This archive is the human desktop analyzer. Cleanup Review and Recycle Bin
maintenance stay human-authorized. There is no agent/CLI mutation surface
in this package's companion CLI archive.

  spacelens-gui.exe

The official Microsoft Visual C++ Redistributable (x64) is a runtime
prerequisite. Qt runtime libraries in this folder were deployed with
windeployqt --no-compiler-runtime --no-system-d3d-compiler
--no-system-dxc-compiler from the Qt 6.8.3 shared kit.

v0.1.0 prerelease binaries are unsigned.

Public redistribution of this GUI package additionally requires:
  - an explicit project license decision (root LICENSE)
  - a structured Qt review PASS with maintainer-controlled
    corresponding source (see docs/QT_REDIST_AUDIT.md)

licenses\ holds verbatim GNU LGPL-3.0 and GPL-3.0 texts as notices of
Qt's license options. They do not complete an LGPL source offer.

Until those are satisfied, treat this zip as a private verification
artifact, not a public release. See THIRD_PARTY_NOTICES.txt.
