# WinGet manifests

Staged community-repository manifests for two distribution profiles of
the same SpaceLens product:

| Identifier | Name | Asset |
| --- | --- | --- |
| `tungcorn.SpaceLens` | SpaceLens | Unified zip (GUI + read-only CLI + Qt runtime) |
| `tungcorn.SpaceLens.CLI` | SpaceLens CLI | Optional CLI-only zip (`spacelens.exe`) |

Schema: ManifestVersion **1.12.0** (current winget-pkgs PR template), multi-file.
These files are the SpaceLens source of truth. Upstream copies live in
`microsoft/winget-pkgs` after review.

Do not advertise `winget install --id …` until the identifier resolves from
the public `winget` source.

Do not install both packages at once. Both expose the `spacelens` command.

`main/` is the complete 0.1.1 product (two NestedInstallerFiles). `cli/`
is the optional CLI-only 0.1.1 package. The `gui/` folder still holds the
superseded v0.1.0 GUI-only manifests. Do not submit those.

Installer URLs and SHA-256 values must match the public GitHub Release.
Do not point them at `releases/latest`, Actions artifacts, or local
`dist/` files.

Published v0.1.1 hashes:

```text
f42252d8303f45c6f7d7f52ad3d9862f3aa94b88566a969d47d21a276bb81c23  spacelens-cli-v0.1.1-windows-x64.zip
b4d4cb993bb53e1414c9fc156d9c29a5dca1b8640ac8d3b1229e5ff5a345793d  spacelens-v0.1.1-windows-x64.zip
```

## Validate locally

```powershell
winget settings --enable LocalManifestFiles
winget validate --manifest packaging\winget\cli
winget validate --manifest packaging\winget\main
```

## Install from the staged manifests

```powershell
winget install --manifest packaging\winget\cli
winget install --manifest packaging\winget\main
```

The complete package aliases are `spacelens` and `spacelens-gui`.
The CLI-only alias is `spacelens`.
WinGet portable packages do **not** create a Start Menu or desktop shortcut.

Local-manifest installs are **user** scope. Uninstall by name works:

```powershell
winget uninstall --name "SpaceLens CLI"
winget uninstall --name SpaceLens
```

Uninstall removes the package files. It does not delete
`%LOCALAPPDATA%\SpaceLens\` index/review state.

## Upstream path

Community repo layout:

```text
manifests/t/tungcorn/SpaceLens/0.1.1/
manifests/t/tungcorn/SpaceLens/CLI/0.1.1/
```

One package identifier per pull request.

## Historical v0.1.0 hashes

Published v0.1.0 assets (immutable, do not reuse for 0.1.1):

```text
2efc2597215b0aba17edd89ef6273f92f3862ab28bc7ec8b718b99c6493fda34  spacelens-cli-v0.1.0-windows-x64.zip
9da45e0fa9eedf45af71a637d971c122fbc093c22650e3be6ec0874ca7a334c3  spacelens-gui-v0.1.0-windows-x64.zip
```
