# WinGet manifests

Staged community-repository manifests for two packages:

| Identifier | Name | Asset |
| --- | --- | --- |
| `tungcorn.SpaceLens` | SpaceLens | GUI zip (Qt runtime included) |
| `tungcorn.SpaceLens.CLI` | SpaceLens CLI | CLI zip (`spacelens.exe` only) |

Schema: ManifestVersion **1.12.0** (current winget-pkgs PR template), multi-file. These files are the SpaceLens
source of truth. Upstream copies live in `microsoft/winget-pkgs` after review.

Do not advertise `winget install --id …` until the identifier resolves from
the public `winget` source.

## Validate locally

```powershell
winget settings --enable LocalManifestFiles
winget validate --manifest packaging\winget\cli
winget validate --manifest packaging\winget\gui
```

## Install from the staged manifests

```powershell
winget install --manifest packaging\winget\cli
winget install --manifest packaging\winget\gui
```

The CLI command alias is `spacelens`. The GUI alias is `spacelens-gui`.
WinGet portable packages do **not** create a Start Menu or desktop shortcut.
Launch the GUI with `spacelens-gui` after a new shell, or from
`%LOCALAPPDATA%\Microsoft\WinGet\Packages\`.

Local-manifest installs are **user** scope. `winget list` may show an ARP
id such as `ARP\User\X64\tungcorn.SpaceLens.CLI__DefaultSource` until the
package is published to the community source. Uninstall by name works:

```powershell
winget uninstall --name "SpaceLens CLI"
winget uninstall --name SpaceLens
```

Uninstall removes the package files. It does not delete
`%LOCALAPPDATA%\SpaceLens\` index/review state.

## Upstream path

Community repo layout:

```text
manifests/t/tungcorn/SpaceLens.CLI/0.1.0/
manifests/t/tungcorn/SpaceLens/0.1.0/
```

One package identifier per pull request.

## v0.1.0 hashes

Taken from the public GitHub Release, not from local `dist/`:

```text
2efc2597215b0aba17edd89ef6273f92f3862ab28bc7ec8b718b99c6493fda34  spacelens-cli-v0.1.0-windows-x64.zip
9da45e0fa9eedf45af71a637d971c122fbc093c22650e3be6ec0874ca7a334c3  spacelens-gui-v0.1.0-windows-x64.zip
```
