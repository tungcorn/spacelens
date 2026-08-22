# Third-party components

This is an inventory, not legal advice.

SpaceLens-owned code is MIT. See the root `LICENSE`. MIT does not apply to
Qt, Qt's bundled third-party code, SQLite, miniz, or pugixml; each component
retains its own terms described below.

## SpaceLens

| Field | Value |
|-------|--------|
| License | MIT |
| SPDX | MIT |
| Copyright | Copyright (c) 2026 tungcorn |
| Text | root `LICENSE` (also copied into both zip archives) |

The maintainer selected MIT. An assistant did not choose this license.

## SQLite

| Field | Value |
|-------|--------|
| Type | vendored source dependency |
| Path | `third_party/sqlite/` |
| Version | 3.53.4 (`sqlite-amalgamation-3530400`) |
| Macros | `SQLITE_VERSION "3.53.4"`, `SQLITE_VERSION_NUMBER 3053004` |
| Source ID | `2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc` |
| Upstream | https://www.sqlite.org/download.html |
| Status | author blessing; not a SpaceLens license and not Qt |

Compiled into `spacelens_core` and therefore into both the CLI and the GUI.

## miniz

| Field | Value |
|-------|--------|
| Type | vendored source dependency; statically linked |
| Path | `third_party/miniz/` |
| Version | 3.1.2 |
| Commit | `77d0dce8627735138c51770d1799a1ef48f2117d` |
| Official source ZIP SHA-256 | `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a` |
| Upstream | https://github.com/richgel999/miniz |
| License | MIT; `third_party/miniz/LICENSE` |
| Build options | `MINIZ_NO_STDIO=1`, `MINIZ_NO_TIME=1`, `MINIZ_NO_ARCHIVE_WRITING_APIS=1` |

SpaceLens uses miniz only through bounded in-memory/random-access callbacks for
ZIP structure, decompression, and CRC validation. The build exposes no miniz
filesystem extraction or archive-writing API and ships no miniz executable or
runtime DLL.

## pugixml

| Field | Value |
|-------|--------|
| Type | vendored source dependency; statically linked |
| Path | `third_party/pugixml/` |
| Version | 1.16 |
| Commit | `c8033ce9d039e7f9d134877c363397b3cfe20816` |
| Official source ZIP SHA-256 | `42e324b50d53aff0cf259a7f26cd04d00f90a1d436b356dd52b0f4d4dcf3b769` |
| Upstream | https://github.com/zeux/pugixml |
| License | MIT; `third_party/pugixml/LICENSE.md` |
| Build options | `PUGIXML_NO_XPATH=1` |

SpaceLens uses pugixml only for size-, depth-, and node-bounded in-memory OOXML
package validation. It ships no pugixml executable or runtime DLL.

Both parser libraries inherit the selected MSVC `/MD` or `/MDd` runtime from
the enclosing build. They are linked into `spacelens_core`, so their code is
present in the CLI, MCP adapter, and GUI without adding runtime files.

## Shipped in the CLI archive

| Component | Location | Notes |
|-----------|----------|--------|
| SpaceLens | `LICENSE` | MIT |
| SQLite amalgamation 3.53.4 | compiled in | blessing in `sqlite3.h`; see `third_party/sqlite/README.md` |
| miniz 3.1.2 | compiled in | MIT; bounded read-only ZIP validation |
| pugixml 1.16 | compiled in | MIT; bounded in-memory OOXML XML validation |
| MSVC runtime | not shipped | Official Microsoft Visual C++ Redistributable (x64) is a prerequisite |

The CLI archive must not contain Qt DLLs, `platforms\`, or the GUI executable.

## Qt

| Field | Value |
|-------|--------|
| Type | dynamically distributed runtime dependency (GUI zip only) |
| Version | 6.8.3 |
| Kit | `win64_msvc2022_64` shared (`qconfig.pri`: `QT_VERSION=6.8.3`, `QT_ARCH=x86_64`, enabled `shared`, disabled `static`) |
| Kit compiler | MSVC 19.39.33520 |
| Modification | unmodified stock kit binaries (aqtinstall / official kit). SpaceLens does not rebuild Qt |
| Direct dumpbin dependents | `Qt6Widgets.dll`, `Qt6Gui.dll`, `Qt6Core.dll` |
| Also deployed | `Qt6Network.dll`, `Qt6Svg.dll`, `opengl32sw.dll`, plugins listed below |
| Not a dumpbin dependent | `Qt6Concurrent.dll` (CMake still links `Qt6::Concurrent`; templates can inline) |
| License basis (kit SPDX) | `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only` |
| Distribution option used | LGPL-3.0-only (with GPL-3.0 as required by LGPL-3.0) |
| Corresponding source | maintainer-controlled written offer + pinned official archive; see `docs/QT_SOURCE_OFFER.md` |

### Shipped in the GUI archive

| Component | How it gets there | Notes |
|-----------|-------------------|--------|
| Qt 6.8.3 shared kit | `windeployqt --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --release` | Dynamic Widgets/Gui/Core runtime and plugins |
| `platforms\qwindows.dll` | required by windeployqt | Packaging fails if this file is missing |
| `Qt6Network.dll`, `Qt6Svg.dll` | windeployqt (plugin dependents) | Allowed; not every plugin name is hardcoded |
| `opengl32sw.dll` | Qt kit `bin\` | Mesa software rasterizer. Size matches the kit, not a compiler CRT |
| SQLite 3.53.4 | linked via `spacelens_core` | Same blessing as the CLI |
| miniz 3.1.2 | linked via `spacelens_core` | MIT; static, no stdio/archive-writing APIs |
| pugixml 1.16 | linked via `spacelens_core` | MIT; static, XPath disabled |
| MSVC runtime | not shipped | Same Visual C++ Redistributable prerequisite |

Windows SDK `d3dcompiler_47.dll` (`4741488` bytes), `dxcompiler.dll`, and
`dxil.dll` are **not** shipped.

GPL-only entries in the kit SPDX set are **build tools** (`moc`, `rcc`,
`uic`, `qmake`, `windeployqt`). They are not in the zip.

Module/plugin table and bundled third-party concluded licenses:
[`docs/QT_REDIST_AUDIT.md`](QT_REDIST_AUDIT.md).
Completed review: [`docs/QT_REDIST_REVIEWED.md`](QT_REDIST_REVIEWED.md).

Machine-checkable record: `packaging/qt-redist-review.env`.

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
- No Qt corresponding source archive inside the GUI zip
