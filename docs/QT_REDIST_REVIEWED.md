# Qt 6.8.3 redistribution review

```
review_status: PASS
spacelens_version: 0.1.0
reviewed_commit: pending-local
qt_version: 6.8.3
architecture: x86_64
linkage: shared
source_availability: READY
review_date: 2026-08-13
```

This is an engineering/compliance evidence review. It is not legal advice.
It is not a SpaceLens MIT grant over Qt.

`reviewed_commit` is filled with the git SHA of the commit that lands
this review. The machine-checkable gate is
`packaging/qt-redist-review.env`, not the presence of this file.

## Exact Qt kit

| Field | Evidence |
|-------|----------|
| Version | 6.8.3 (`qconfig.pri` `QT_VERSION`, CI `QT_VERSION`, `scripts/install-qt.ps1`) |
| Kit | `win64_msvc2022_64` / aqtinstall archive name |
| Architecture | `x86_64` (`QT_ARCH`) |
| Linkage | shared (`QT.global.enabled_features` includes `shared`; `static` is disabled) |
| Kit MSVC | 19.39.33520 |
| Consumer MSVC | 19.50 locally; hosted windows-2022 in CI. Links the shared kit; does not rebuild Qt |
| Provenance | Official Qt 6.8.3 MSVC 2022 64-bit kit via aqtinstall / local `CMAKE_PREFIX_PATH` |
| SPDX | kit `sbom/` SPDX 2.3 |

## Modification status

Packaged Qt binaries are **stock Qt 6.8.3 kit binaries**. SpaceLens has
no Qt source build, no patch series, and no rebuilt `Qt6*.dll`.
`scripts/package-release.ps1` copies files from the selected kit with
`windeployqt`.

## Dynamic linkage

`dumpbin /dependents` on Release `spacelens-gui.exe` (2026-08-13):

```
Qt6Widgets.dll
Qt6Gui.dll
Qt6Core.dll
```

plus Windows system libraries and the MSVC runtime (`MSVCP140.dll`,
`MSVCP140_ATOMIC_WAIT.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`).
Those runtime DLLs are **not** shipped.

`dumpbin /dependents` on Release `spacelens.exe` lists no `Qt6*.dll`.

CMake also links `Qt6::Concurrent`. `Qt6Concurrent.dll` is **not** a
dumpbin dependent of this GUI and is not deployed.

## Actual runtime inventory

Direct linked / dumpbin modules: Qt6Core, Qt6Gui, Qt6Widgets.

Shipped DLLs (fresh windeployqt after
`--no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler
--release`):

- `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`
- `Qt6Network.dll`, `Qt6Svg.dll` (plugin dependents)
- `opengl32sw.dll` (Mesa llvmpipe from the kit `bin\`, 20639888 bytes)

Shipped plugins:

- `platforms\qwindows.dll` (required)
- `styles\qmodernwindowsstyle.dll`
- `imageformats\qgif.dll`, `qico.dll`, `qjpeg.dll`, `qsvg.dll`
- `iconengines\qsvgicon.dll`
- `generic\qtuiotouchplugin.dll`
- `networkinformation\qnetworklistmanager.dll`
- `tls\qcertonlybackend.dll`, `qschannelbackend.dll`

Other: `translations\qt_*.qm`, SpaceLens `LICENSE`,
`THIRD_PARTY_NOTICES.txt`, `licenses\LGPL-3.0.txt`,
`licenses\GPL-3.0.txt`, `licenses\QT_SOURCE_IDENTITY.txt`,
`licenses\QT_SOURCE_OFFER.md`.

Not shipped: `moc`/`rcc`/`uic`/`qmake`/`windeployqt`,
`Qt6Concurrent.dll`, Windows SDK D3D/DXC compilers, MSVC CRT DLLs,
PDBs, tests, `state.db`, Qt headers/libs.

## License basis

Every shipped Qt module and plugin above is concluded by the 6.8.3 kit
SPDX as:

`LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`

SpaceLens distributes those libraries under the **LGPL-3.0-only**
option. GPL-only SPDX entries in the kit are build tools and are not
in the zip.

**GPL-only runtime conflict:** none observed for shipped files.

## Third-party code inside shipped Qt

From the kit SPDX (not an independent source audit): PCRE2, zlib,
blake2, tinycbor, double-conversion, libpng, libjpeg (IJG notice
required and included), freetype (FTL available), harfbuzz, psl-data,
libpsl, md4c, wintab. Mesa llvmpipe (`opengl32sw.dll`) is MIT + Boost
1.0 per Qt attribution.

## Included texts

- SpaceLens `LICENSE` (MIT) — project code only
- `THIRD_PARTY_NOTICES.txt`
- `licenses/LGPL-3.0.txt` and `licenses/GPL-3.0.txt` (verbatim gnu.org)
- `licenses/QT_SOURCE_IDENTITY.txt`
- `licenses/QT_SOURCE_OFFER.md`

## Corresponding source

Maintainer-controlled written offer:

- Identity pin: `packaging/qt-source/SOURCE_IDENTITY.txt`
- Official archive: `qt-everywhere-src-6.8.3.tar.xz`
- SHA-256: `cdd3a69967208276bb01af7ace7dba0ba53e679f886a4cbe624225c60fb73f2c`
  copied from
  https://download.qt.io/official_releases/qt/6.8/6.8.3/single/qt-everywhere-src-6.8.3.tar.xz.sha256
  on 2026-08-13
- Offer text: `docs/QT_SOURCE_OFFER.md` (also packaged)
- Contact: GitHub issues on https://github.com/tungcorn/spacelens/issues
  with title prefix `[Qt corresponding source]`
- Duration: at least three years after each distribution of these binaries
- Fulfillment: `scripts/fetch-qt-corresponding-source.ps1` verifies the pin

Upstream download.qt.io is a retrieval convenience. The pin, offer, and
fulfillment script are SpaceLens-controlled. The ~1 GB archive is **not**
inside the GUI runtime zip.

## Relinking / replacement

External Qt DLLs and plugin directories sit beside `spacelens-gui.exe`.
No DLL signature allowlist, encrypted Qt container, anti-replacement
loader, or DRM. Recipients may replace compatible Qt libraries. This is
not a promise of compatibility beyond Qt's own interface/ABI guarantees.

## VC++ runtime policy

`windeployqt --no-compiler-runtime`. No `vcruntime*.dll` / `msvcp*.dll`
in the zip. Official Microsoft Visual C++ Redistributable (x64) is a
prerequisite.

## Version bind

This review is valid only for Qt **6.8.3** shared. `scripts/verify-qt-redist-review.ps1`
fails if `qt_version` is not `6.8.3`, if `linkage` is not `shared`, or if
`source_availability` is not `READY` when `review_status=PASS`.
