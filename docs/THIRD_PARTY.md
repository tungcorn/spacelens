# Third-party components

This is an inventory, not a redistribution grant.

## Shipped in the CLI archive

| Component | Location | Notes |
|-----------|----------|--------|
| SQLite amalgamation 3.53.4 | `third_party/sqlite/` | Compiled into `spacelens_core`. Blessing in `sqlite3.h`; see `third_party/sqlite/README.md`. Independent of the SpaceLens project license and of Qt. |
| MSVC runtime | not shipped | Official Microsoft Visual C++ Redistributable (x64) is a prerequisite. |

The CLI archive must not contain Qt DLLs, `platforms\`, or the GUI executable.

## Shipped in the GUI archive (private verification only)

| Component | How it gets there | Notes |
|-----------|-------------------|--------|
| Qt 6.8.3 shared kit | `windeployqt --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --release` | Dynamic Widgets/Gui/Core runtime and plugins. Binary proof: `dumpbin /dependents` lists `Qt6Widgets.dll`, `Qt6Gui.dll`, `Qt6Core.dll`. |
| `platforms\qwindows.dll` | required by windeployqt | Packaging fails if this file is missing. |
| `Qt6Network.dll`, `Qt6Svg.dll` | windeployqt (plugin dependents) | Allowed; not every plugin name is hardcoded. |
| `opengl32sw.dll` | Qt kit `bin\` | Mesa software rasterizer. Size matches the kit, not a compiler CRT. |
| SQLite 3.53.4 | linked via `spacelens_core` | Same blessing as the CLI. |
| MSVC runtime | not shipped | Same Visual C++ Redistributable prerequisite. No `vcruntime*.dll` / `msvcp*.dll` in the zip. |

Windows SDK `d3dcompiler_47.dll` (`4741488` bytes), `dxcompiler.dll`, and
`dxil.dll` are **not** shipped. Default windeployqt pulled them from
`Windows Kits\10\Redist\D3D\x64`; the extra flags stop that.

Kit SPDX (SPDX 2.3 under `<qt-prefix>/sbom/`) concludes the shipped Qt
modules and plugins as:

`LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`

GPL-only entries in that SPDX set are **build tools** (`moc`, `rcc`,
`uic`, `qmake`, `windeployqt`). They are not in the zip.

Full module/plugin table, bundled third-party concluded licenses, and
corresponding-source status: [`docs/QT_REDIST_AUDIT.md`](QT_REDIST_AUDIT.md).

**Qt redistribution status: PENDING.**
`packaging/qt-redist-review.env` has `review_status=PENDING`,
`qt_version=6.8.3`, `linkage=shared`, `source_availability=MISSING`.

There is no maintainer-controlled Qt corresponding-source archive or
offer (`QT_SOURCE_AVAILABILITY_REQUIRED`). Do not create
`docs/QT_REDIST_REVIEWED.md`.

`packaging/gui/licenses/LGPL-3.0.txt` and `GPL-3.0.txt` are verbatim GNU
texts packaged as notices. They do not complete an LGPL source offer.

Public GUI distribution stays blocked until a maintainer:

1. chooses a project `LICENSE`
2. provides a real corresponding-source mechanism and sets
   `source_availability=READY`
3. sets `review_status=PASS` only after a completed review of **this**
   6.8.3 shared kit

A Qt-free CLI is not technically blocked by the unfinished Qt review.
A joint public prerelease of both zips still needs both gates. See
[`docs/RELEASING.md`](RELEASING.md).

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
- No Qt corresponding source archive
- No project `LICENSE`
