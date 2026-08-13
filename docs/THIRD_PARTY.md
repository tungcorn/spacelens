# Third-party components

This is an inventory, not a redistribution grant.

## Shipped in the CLI archive

| Component | Location | Notes |
|-----------|----------|--------|
| SQLite amalgamation | `third_party/sqlite/` | Compiled into `spacelens_core`. See `third_party/sqlite/README.md`. |
| MSVC runtime | not shipped | Official Microsoft Visual C++ Redistributable (x64) is a prerequisite. |

The CLI archive must not contain Qt DLLs, `platforms\`, or the GUI executable.

## Shipped in the GUI archive (private verification only)

| Component | How it gets there | Notes |
|-----------|-------------------|--------|
| Qt 6.8.3 (pinned) | `windeployqt --no-compiler-runtime --release` | Dynamic Qt 6 Widgets / Concurrent runtime and plugins. |
| `platforms\qwindows.dll` | required by windeployqt | Packaging fails if this file is missing. |
| Qt6Core / Gui / Widgets / Network / Svg | windeployqt | Observed in the local v0.1.0 GUI stage. |
| `opengl32sw.dll`, `d3dcompiler_47.dll`, `dxcompiler.dll`, `dxil.dll` | windeployqt from the Qt kit | Software rasterizer / D3D shader support. Not MSVC CRT. |
| MSVC runtime | not shipped | Same Visual C++ Redistributable prerequisite as the CLI. No `vcruntime*.dll` / `msvcp*.dll` in the zip. |

Qt is licensed by The Qt Company under LGPL v3, GPL, or a commercial license,
depending on how the developer obtained the kit. SpaceLens does **not** currently
document which Qt license applies to a given build machine, does **not** ship
corresponding Qt source or offer text, and does **not** claim LGPL compliance
for public redistribution.

**Qt redistribution status: not maintainer-reviewed.**

Public GUI distribution is `RELEASE_DISTRIBUTION_BLOCKED` until a maintainer:

1. chooses a project `LICENSE`
2. records the Qt kit license actually used
3. writes `docs/QT_REDIST_REVIEWED.md` after completing that review

Do not create `docs/QT_REDIST_REVIEWED.md` as a formality.

## Used to build, not shipped

| Component | Pin | Purpose |
|-----------|-----|---------|
| CMake ≥ 3.21 | CMakePresets schema 3 | Configure / build / install |
| Ninja | host tool | Generator |
| MSVC + Windows SDK | host tool | Compiler |
| Qt 6.8.3 win64_msvc2022_64 | exact version, not `6.*` | GUI only |
| aqtinstall 3.3.0 | `scripts/install-qt.ps1` | CI Qt installer |
| GitHub Actions `actions/checkout` | `3d3c42e5aac5ba805825da76410c181273ba90b1` (v7.0.1) | CI checkout |
| GitHub Actions `actions/upload-artifact` | `043fb46d1a93c77aae656e7c1c64a875d1fc6a0a` (v7.0.1) | CI artifacts |
| GitHub Actions `actions/download-artifact` | `3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c` (v8.0.1) | Release download |

Action SHAs were read from the GitHub git-refs API for the named tags. Do not
replace a pin with a floating major tag.

## Not present

- No committed Visual C++ runtime DLLs
- No certificates, `.pfx` files, private keys, or passwords
- No self-signed Authenticode material
