# Written offer of Qt 6.8.3 corresponding source

This is a maintainer-controlled written offer for the Qt libraries
dynamically shipped with SpaceLens GUI 0.1.0. It is not legal advice.
It is not the SpaceLens MIT license. Qt remains under the licenses
declared by the Qt 6.8.3 kit.

## Offer

The SpaceLens maintainer offers to give you a copy of the Corresponding
Source for the Qt 6.8.3 modules and plugins actually distributed with
`spacelens-gui-v0.1.0-windows-x64.zip`, for as long as SpaceLens continues
to distribute those binaries and for at least three years after each
such distribution.

The source offered is the official Qt 6.8.3 open-source source archive
identified in `packaging/qt-source/SOURCE_IDENTITY.txt`:

- archive: `qt-everywhere-src-6.8.3.tar.xz`
- SHA-256: `cdd3a69967208276bb01af7ace7dba0ba53e679f886a4cbe624225c60fb73f2c`
- official URL recorded for retrieval:
  https://download.qt.io/official_releases/qt/6.8/6.8.3/single/qt-everywhere-src-6.8.3.tar.xz

The SpaceLens project does not modify Qt. Packaged `Qt6*.dll` files and
plugins are stock binaries from the Qt 6.8.3 `win64_msvc2022_64` shared
kit.

## How to request it

Open an issue on the SpaceLens repository with the title prefix:

```text
[Qt corresponding source]
```

Repository: https://github.com/tungcorn/spacelens/issues

Ask for the Qt 6.8.3 corresponding source identified above. The maintainer
will provide the verified archive, or equivalent access to that exact
SHA-256, at no more than the reasonable cost of physically performing
the conveyance.

## How the maintainer fulfills a request

From a clone of this repository:

```powershell
.\scripts\fetch-qt-corresponding-source.ps1 -OutDir <directory>
```

The script downloads the official archive and refuses to accept a file
whose SHA-256 does not match the pin. The pin lives in this repository,
so the identity is under SpaceLens maintainer control. Upstream
availability on download.qt.io is a retrieval convenience, not the offer
itself.

Do not put the ~1 GB source archive inside the GUI runtime zip.

## What this is not

- Not a claim that Qt is MIT.
- Not a substitute for the GNU LGPL-3.0 / GPL-3.0 texts in `licenses/`.
- Not a promise that a rebuilt Qt will be ABI-compatible beyond Qt's
  own guarantees.
- Not permission to treat download.qt.io alone as SpaceLens compliance.
