# Qt 6.8.3 redistribution audit

Inventory of the shipped Qt 6.8.3 shared runtime. The completed
machine-checkable review is `packaging/qt-redist-review.env`. The
human narrative is [`QT_REDIST_REVIEWED.md`](QT_REDIST_REVIEWED.md).

This is an engineering inventory. It is not legal advice. It is not
the SpaceLens MIT license. Qt is not MIT.

## Scope

| Artifact | Qt? | This audit |
|----------|-----|------------|
| `spacelens-cli-v*-windows-x64.zip` | No | Out of scope. CLI is Qt-free. |
| `spacelens-gui-v*-windows-x64.zip` | Yes | In scope. |

## Kit identity

Pinned everywhere as **Qt 6.8.3** `win64_msvc2022_64` (aqtinstall archive
name). Not `6.*`.

| Evidence | Value |
|----------|--------|
| `scripts/install-qt.ps1` / CI `QT_VERSION` | `6.8.3` |
| `qconfig.pri` `QT_VERSION` | `6.8.3` |
| `qconfig.pri` `QT_ARCH` | `x86_64` |
| `qconfig.pri` enabled | `shared` |
| `qconfig.pri` disabled | `static` |
| Kit built with MSVC | `19.39.33520` |
| Local consumer MSVC (this machine) | `19.50` (links the shared kit, does not rebuild Qt) |
| SPDX | `<qt-prefix>/sbom/` SPDX 2.3, including `qtbase-6.8.3.spdx.json` |

CMake `find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)` and
`target_link_libraries(SpaceLens PRIVATE Qt6::Widgets Qt6::Concurrent)`
select this shared kit when `CMAKE_PREFIX_PATH` points at it.

## Dynamic linkage (binary proof)

`dumpbin /dependents` on Release `spacelens-gui.exe` (not CMake link
lines) lists:

```
Qt6Widgets.dll
Qt6Gui.dll
Qt6Core.dll
```

plus Windows system libraries and the MSVC runtime
(`MSVCP140.dll`, `MSVCP140_ATOMIC_WAIT.dll`, `VCRUNTIME140.dll`,
`VCRUNTIME140_1.dll`). Those runtime DLLs are **not** staged.

`dumpbin /dependents` on Release `spacelens.exe` lists **no** `Qt6*.dll`.

`Qt6Concurrent.dll` exists in the kit (`35464` bytes) but is **not** a
dumpbin dependent of `spacelens-gui.exe`. Qt Concurrent is consumed
mostly via headers. Packaging must not require that DLL.

This is a **shared/dynamic** Qt build. It is not a static Qt link.

## windeployqt flags

`scripts/package-release.ps1` deploys with:

```text
windeployqt --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --release
```

| Flag | Why |
|------|-----|
| `--no-compiler-runtime` | Do not copy `vcruntime*.dll` / `msvcp*.dll` from the developer machine. Official Microsoft Visual C++ Redistributable (x64) is the prerequisite. |
| `--no-system-d3d-compiler` | Default windeployqt copied Windows SDK `Redist\D3D\x64\d3dcompiler_47.dll` (`4741488` bytes), not the Qt kit copy (`4173928` bytes). |
| `--no-system-dxc-compiler` | Default windeployqt copied Windows SDK `dxcompiler.dll` (`14316920`) and `dxil.dll` (`1509744`). Those files are **not** in the Qt kit `bin\`. |

`opengl32sw.dll` (`20639888` bytes) matches the Qt kit `bin\` copy and
is kept (Mesa llvmpipe software rasterizer shipped with the kit).

A fresh Release package after those flags contains **no**
`d3dcompiler_47.dll`, `dxcompiler.dll`, or `dxil.dll`.

## Shipped Qt modules and plugins (GUI zip)

Direct dumpbin dependents (must be present):

| File | Kit SPDX concluded license |
|------|----------------------------|
| `Qt6Core.dll` | `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only` |
| `Qt6Gui.dll` | same |
| `Qt6Widgets.dll` | same |

Typically also deployed because plugins pull them (allowed, not every
name is hardcoded in the validator):

| File | Kit SPDX concluded license |
|------|----------------------------|
| `Qt6Network.dll` | same LGPL-capable expression |
| `Qt6Svg.dll` | same |

Plugins observed from this kit / windeployqt (all LGPL-capable in the
6.8.3 kit SPDX):

| Deployed file | SPDX package |
|---------------|--------------|
| `platforms\qwindows.dll` | QWindowsIntegrationPlugin |
| `styles\qmodernwindowsstyle.dll` | QModernWindowsStylePlugin |
| `imageformats\qgif.dll` | QGifPlugin |
| `imageformats\qico.dll` | QICOPlugin |
| `imageformats\qjpeg.dll` | QJpegPlugin |
| `imageformats\qsvg.dll` | QSvgPlugin |
| `iconengines\qsvgicon.dll` | QSvgIconPlugin |
| `generic\qtuiotouchplugin.dll` | QTuioTouchPlugin |
| `networkinformation\qnetworklistmanager.dll` | QNLMNIPlugin |
| `tls\qcertonlybackend.dll` | QTlsBackendCertOnlyPlugin |
| `tls\qschannelbackend.dll` | QSchannelBackendPlugin |

`platforms\qwindows.dll` is required. Packaging fails without it.

`translations\qt_*.qm` come from the kit. They stay in the zip. They are
not a substitute for Qt corresponding source.

## Not shipped (do not audit as if they were)

| Item | Why excluded |
|------|----------------|
| `moc`, `rcc`, `uic`, `qmake`, `windeployqt` | Build tools. Kit SPDX marks several as GPL-only. They are not in the GUI zip. |
| `Qt6Concurrent.dll` | Not a runtime dependent of this GUI. |
| Windows SDK `d3dcompiler_47.dll` / `dxcompiler.dll` / `dxil.dll` | Excluded by windeployqt flags. Validator rejects the SDK-sized D3D compiler and any `dxcompiler.dll` / `dxil.dll`. |
| `vcruntime*.dll`, `msvcp*.dll` | Not staged. Redistributable prerequisite. |
| Headers, `.lib`, CMake packages, PDBs, `state.db` | Not installed. Validator rejects them. |
| `spacelens.exe` | CLI is a separate archive. |

## Bundled third-party code inside shipped Qt modules

Concluded licenses below are copied from the **Qt 6.8.3 kit SPDX** for
packages that feed the shipped modules. This is not an independent
line-by-line source audit.

| Component | SPDX concluded (kit) |
|-----------|----------------------|
| PCRE2 | BSD-3-Clause with PCRE2 exception |
| zlib | Zlib |
| blake2 | CC0-1.0 OR Apache-2.0 |
| tinycbor | MIT |
| double-conversion | BSD-3-Clause |
| libpng | libpng / zlib-style |
| libjpeg | IJG AND BSD-3-Clause |
| freetype | FTL OR GPL-2.0-only (FTL available) |
| harfbuzz | MIT |
| psl-data | MPL-2.0 |
| libpsl | BSD-3-Clause |
| md4c | MIT |
| wintab | LicenseRef-Lcs-Telegraphics |

If a future kit SPDX changes a concluded license for a **shipped**
module to GPL-only with no LGPL/commercial alternative, this audit is
invalid for that kit.

## Corresponding source

See [`QT_SOURCE_OFFER.md`](QT_SOURCE_OFFER.md) and
`packaging/qt-source/SOURCE_IDENTITY.txt`.

Pinned official archive:

```text
qt-everywhere-src-6.8.3.tar.xz
SHA-256 cdd3a69967208276bb01af7ace7dba0ba53e679f886a4cbe624225c60fb73f2c
https://download.qt.io/official_releases/qt/6.8/6.8.3/single/qt-everywhere-src-6.8.3.tar.xz
```

The SHA-256 is copied from Qt's official `.sha256` file on 2026-08-13.
The pin and the written offer live in this repository. Recipients request
fulfillment via GitHub issues. The maintainer fulfills with
`scripts/fetch-qt-corresponding-source.ps1`, which refuses a hash
mismatch.

Do not put the full Qt source archive inside the GUI zip.

## Relinking

`spacelens-gui.exe` loads stock shared Qt DLLs from the application
directory and plugin subdirectories. There is no DLL signature
allowlist, encrypted Qt container, anti-replacement loader, or DRM.
Users may replace compatible Qt DLLs. SpaceLens does not promise ABI
compatibility beyond Qt's own guarantees.

## Project license interaction

SpaceLens is MIT. That license covers SpaceLens-owned code only. It
does not make Qt MIT and does not relicense SQLite.
