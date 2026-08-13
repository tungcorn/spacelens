# Qt 6.8.3 redistribution audit

**Status: PENDING — not a redistribution grant.**

This file is evidence for a future maintainer review. It is **not**
`docs/QT_REDIST_REVIEWED.md`. Presence of this file must not unlock a
public GUI release.

Machine-checkable record: `packaging/qt-redist-review.env`.

```
review_status=PENDING
qt_version=6.8.3
linkage=shared
source_availability=MISSING
```

`scripts/verify-qt-redist-review.ps1 -RequirePass` exits non-zero on this
record. A GitHub Release must not treat `Test-Path` of this document, of
an empty sentinel, or of `docs/QT_REDIST_REVIEWED.md` as approval.

This is an engineering inventory. It is not legal advice.

## Scope

| Artifact | Qt? | This audit |
|----------|-----|------------|
| `spacelens-cli-v*-windows-x64.zip` | No | Out of scope. CLI is Qt-free. |
| `spacelens-gui-v*-windows-x64.zip` | Yes | In scope. |

A Qt-free CLI must not stay blocked solely because this GUI audit is
unfinished. Combined public publication of **both** zips still requires
a maintainer-chosen project `LICENSE` **and** a structured review PASS.
See `docs/RELEASING.md`.

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
`d3dcompiler_47.dll`, `dxcompiler.dll`, or `dxil.dll`. windeployqt
reports direct dependents `Qt6Core Qt6Gui Qt6Widgets` and deploys
those plus `Qt6Network` / `Qt6Svg` for plugins. `Qt6Concurrent.dll`
is not deployed: this GUI's dumpbin dependents do not include it,
even though CMake links `Qt6::Concurrent` (templates can inline).

## Shipped Qt modules and plugins (GUI zip)

Inventory is of a **fresh** Release package after the flags above, not of
a leftover `stage\` directory. `scripts/verify-package.ps1` enforces the
required/forbidden categories.

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

LGPL-3.0, if relied on for the shipped Qt libraries, requires
corresponding source and the other applicable LGPL conditions for those
libraries. SpaceLens does **not** currently:

- vendor a Qt 6.8.3 source archive under maintainer control
- publish a written offer that a recipient can actually use
- point at a SpaceLens-controlled URL that hosts that source

The official Qt open-source source drop exists upstream. That is not a
SpaceLens-controlled corresponding-source mechanism.

`source_availability=MISSING`.

Therefore:

- `docs/QT_REDIST_REVIEWED.md` must **not** be created
- `review_status` must stay `PENDING` (or `FAIL`)
- GUI public distribution stays blocked (`QT_SOURCE_AVAILABILITY_REQUIRED`)

`packaging/gui/licenses/LGPL-3.0.txt` and `GPL-3.0.txt` are verbatim GNU
texts included as **notices** of Qt's license options. They do **not**
complete an LGPL corresponding-source offer.

Do not put the full Qt source archive inside the GUI zip automatically.

## Project license interaction

`MAINTAINER_PROJECT_LICENSE` is undecided. An assistant must not choose
one. See `docs/LICENSE_DECISION_REQUIRED.md`.

Even a completed Qt review would not make SpaceLens itself
redistributable until a maintainer writes a real root `LICENSE`.

## What would flip this record to PASS

A **maintainer** (not an assistant guessing) must:

1. Choose a project license and write root `LICENSE`.
2. Confirm this still ships Qt **6.8.3** shared, not static, not another
   version.
3. Re-inventory a fresh GUI zip and confirm no GPL-only-only runtime
   and no Windows SDK D3D/DXC compilers.
4. Put a real corresponding-source mechanism under SpaceLens control
   and set `source_availability=READY`.
5. Set `review_status=PASS` in `packaging/qt-redist-review.env` only
   after 1–4.
6. Only then create `docs/QT_REDIST_REVIEWED.md` as human narrative.

Until then the only honest overall status is
`RELEASE_DISTRIBUTION_BLOCKED`.
