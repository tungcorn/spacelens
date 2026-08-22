# Stage portable Windows x64 zip archives from a Release build.
# Primary archive: unified GUI + read-only CLI + read-only MCP + Qt runtime.
# Headless archive: CLI + MCP (no Qt, no GUI). Filename stays spacelens-cli-*.
# Uses cmake --install components, then windeployqt for the unified tree.

[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$OutDir = "",
    [switch]$SkipSmoke
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root "build-release" }
if (-not $OutDir) { $OutDir = Join-Path $root "dist" }

if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    Write-Error "Release build directory not found: $BuildDir"
}

function Get-ProjectVersion([string]$CachePath) {
    foreach ($line in Get-Content $CachePath) {
        if ($line -match '^CMAKE_PROJECT_VERSION:STATIC=(.+)$') {
            return $Matches[1].Trim()
        }
        if ($line -match '^CMAKE_PROJECT_VERSION:INTERNAL=(.+)$') {
            return $Matches[1].Trim()
        }
    }
    Write-Error "CMAKE_PROJECT_VERSION not found in $CachePath"
}

function Get-CacheValue([string]$CachePath, [string]$Name) {
    $prefix = "$Name`:"
    foreach ($line in Get-Content $CachePath) {
        if ($line.StartsWith($prefix)) {
            $eq = $line.IndexOf("=")
            if ($eq -ge 0) { return $line.Substring($eq + 1) }
        }
    }
    return $null
}

function Get-Windeployqt {
    $fromEnv = $env:CMAKE_PREFIX_PATH
    if ($fromEnv) {
        foreach ($entry in ($fromEnv -split ";")) {
            $candidate = Join-Path $entry "bin\windeployqt.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    $qtDir = Get-CacheValue (Join-Path $BuildDir "CMakeCache.txt") "Qt6_DIR"
    if ($qtDir) {
        # Qt6_DIR is typically <prefix>/lib/cmake/Qt6
        $prefix = Resolve-Path (Join-Path $qtDir "..\..\..") -ErrorAction SilentlyContinue
        if ($prefix) {
            $candidate = Join-Path $prefix.Path "bin\windeployqt.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    $onPath = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    Write-Error "windeployqt.exe not found. Set CMAKE_PREFIX_PATH to the Qt prefix."
}

function New-Sha256Sums([string]$Directory, [string[]]$ZipNames) {
    $out = Join-Path $Directory "SHA256SUMS.txt"
    $lines = @()
    foreach ($name in ($ZipNames | Sort-Object)) {
        $path = Join-Path $Directory $name
        if (-not (Test-Path $path)) {
            Write-Error "checksum zip not found: $path"
        }
        $hash = (Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant()
        $lines += "$hash  $name"
    }
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllLines($out, $lines, $utf8)
    Write-Host "Wrote $out"
    return $out
}

function Copy-ProjectLicense([string]$Destination) {
    $license = Join-Path $root "LICENSE"
    if (-not (Test-Path $license)) {
        Write-Error "root LICENSE is required in staged packages"
    }
    $text = (Get-Content $license -Raw -ErrorAction SilentlyContinue)
    if (-not $text -or $text.Trim().Length -eq 0) {
        Write-Error "root LICENSE is empty"
    }
    Copy-Item $license (Join-Path $Destination "LICENSE")
}

function Copy-ThirdPartyLicenses([string]$Destination) {
    $licenses = Join-Path $Destination "licenses"
    $minizDir = Join-Path $licenses "miniz"
    $pugixmlDir = Join-Path $licenses "pugixml"
    New-Item -ItemType Directory -Force -Path $minizDir, $pugixmlDir | Out-Null

    $copies = @(
        @{
            Source = Join-Path $root "third_party\miniz\LICENSE"
            Destination = Join-Path $minizDir "LICENSE"
        }
        @{
            Source = Join-Path $root "third_party\pugixml\LICENSE.md"
            Destination = Join-Path $pugixmlDir "LICENSE.md"
        }
    )
    foreach ($copy in $copies) {
        if (-not (Test-Path $copy.Source)) {
            Write-Error "third-party license is required: $($copy.Source)"
        }
        $text = Get-Content $copy.Source -Raw -ErrorAction SilentlyContinue
        if (-not $text -or $text.Trim().Length -eq 0) {
            Write-Error "third-party license is empty: $($copy.Source)"
        }
        Copy-Item $copy.Source $copy.Destination -Force
    }
}

$version = Get-ProjectVersion (Join-Path $BuildDir "CMakeCache.txt")
$cliName = "spacelens-cli-v$version-windows-x64"
$mainName = "spacelens-v$version-windows-x64"

$stage = Join-Path $root "stage"
$cliStage = Join-Path $stage $cliName
$mainStage = Join-Path $stage $mainName
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $cliStage, $mainStage, $OutDir | Out-Null

Write-Host "Checking Qt review record (well-formed, not RequirePass)"
& (Join-Path $root "scripts\verify-qt-redist-review.ps1")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Installing headless CLI + MCP components -> $cliStage"
cmake --install $BuildDir --prefix $cliStage --component SpaceLensCli
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --install $BuildDir --prefix $cliStage --component SpaceLensMcp
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item (Join-Path $root "packaging\cli\README.txt") (Join-Path $cliStage "README.txt")
Copy-Item (Join-Path $root "packaging\cli\THIRD_PARTY_NOTICES.txt") (Join-Path $cliStage "THIRD_PARTY_NOTICES.txt")
Copy-ProjectLicense $cliStage
Copy-ThirdPartyLicenses $cliStage

$cliExe = Join-Path $cliStage "spacelens.exe"
$cliMcp = Join-Path $cliStage "spacelens-mcp.exe"
if (-not (Test-Path $cliExe)) {
    Write-Error "headless install did not produce spacelens.exe"
}
if (-not (Test-Path $cliMcp)) {
    Write-Error "headless install did not produce spacelens-mcp.exe"
}

Write-Host "Installing unified main components -> $mainStage"
cmake --install $BuildDir --prefix $mainStage --component SpaceLensGui
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --install $BuildDir --prefix $mainStage --component SpaceLensCli
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --install $BuildDir --prefix $mainStage --component SpaceLensMcp
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item (Join-Path $root "packaging\gui\README.txt") (Join-Path $mainStage "README.txt")
Copy-Item (Join-Path $root "packaging\gui\THIRD_PARTY_NOTICES.txt") (Join-Path $mainStage "THIRD_PARTY_NOTICES.txt")
$mainLicenses = Join-Path $mainStage "licenses"
New-Item -ItemType Directory -Force -Path $mainLicenses | Out-Null
Copy-Item (Join-Path $root "packaging\gui\licenses\*") $mainLicenses -Force
Copy-Item (Join-Path $root "packaging\qt-source\SOURCE_IDENTITY.txt") (Join-Path $mainLicenses "QT_SOURCE_IDENTITY.txt")
Copy-Item (Join-Path $root "docs\QT_SOURCE_OFFER.md") (Join-Path $mainLicenses "QT_SOURCE_OFFER.md")
Copy-ProjectLicense $mainStage
Copy-ThirdPartyLicenses $mainStage

$guiExe = Join-Path $mainStage "spacelens-gui.exe"
$mainCliExe = Join-Path $mainStage "spacelens.exe"
$mainMcpExe = Join-Path $mainStage "spacelens-mcp.exe"
if (-not (Test-Path $guiExe)) {
    Write-Error "unified install did not produce spacelens-gui.exe"
}
if (-not (Test-Path $mainCliExe)) {
    Write-Error "unified install did not produce spacelens.exe"
}
if (-not (Test-Path $mainMcpExe)) {
    Write-Error "unified install did not produce spacelens-mcp.exe"
}

$windeploy = Get-Windeployqt
Write-Host "Deploying Qt runtime with $windeploy"
& $windeploy --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --release --dir $mainStage $guiExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Inventory deployed Qt modules for the third-party audit.
$qtDlls = Get-ChildItem $mainStage -Filter "Qt6*.dll" | Sort-Object Name | ForEach-Object { $_.Name }
Write-Host "Deployed Qt DLLs: $($qtDlls -join ', ')"

& (Join-Path $root "scripts\verify-package.ps1") -CliStage $cliStage -MainStage $mainStage
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$cliZip = Join-Path $OutDir "$cliName.zip"
$mainZip = Join-Path $OutDir "$mainName.zip"
if (Test-Path $cliZip) { Remove-Item -Force $cliZip }
if (Test-Path $mainZip) { Remove-Item -Force $mainZip }

Compress-Archive -Path (Join-Path $cliStage "*") -DestinationPath $cliZip
Compress-Archive -Path (Join-Path $mainStage "*") -DestinationPath $mainZip
$cliZipName = Split-Path $cliZip -Leaf
$mainZipName = Split-Path $mainZip -Leaf
$sums = New-Sha256Sums $OutDir @($cliZipName, $mainZipName)
& (Join-Path $root "scripts\verify-release-checksums.ps1") `
    -SumsPath $sums `
    -ZipDir $OutDir `
    -AttachedNames @($cliZipName, $mainZipName)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Simulate a CLI-only publish set: checksums must name only the CLI zip.
$cliOnlySums = Join-Path $OutDir "SHA256SUMS-cli-only.txt"
$cliLine = Select-String -LiteralPath $sums -SimpleMatch "  $cliZipName" | ForEach-Object { $_.Line }
if (-not $cliLine) { Write-Error "CLI zip missing from SHA256SUMS.txt" }
$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllLines($cliOnlySums, @($cliLine), $utf8)
& (Join-Path $root "scripts\verify-release-checksums.ps1") `
    -SumsPath $cliOnlySums `
    -ZipDir $OutDir `
    -AttachedNames @($cliZipName)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Remove-Item -Force $cliOnlySums

if (-not $SkipSmoke) {
    $extractCli = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-pkg-cli-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $extractCli | Out-Null
    try {
        Expand-Archive -Path $cliZip -DestinationPath $extractCli
        $smoke = Join-Path $extractCli "spacelens.exe"
        $mcpSmoke = Join-Path $extractCli "spacelens-mcp.exe"
        $verOut = & $smoke version
        if ($verOut -notmatch [regex]::Escape($version)) {
            Write-Error "Extracted headless CLI version '$verOut' does not contain $version"
        }
        if (Test-Path (Join-Path $extractCli "spacelens-gui.exe")) {
            Write-Error "headless zip must not contain spacelens-gui.exe"
        }
        if (-not (Test-Path $mcpSmoke)) {
            Write-Error "headless zip missing spacelens-mcp.exe"
        }
        & (Join-Path $root "scripts\verify-cli-safety.ps1") -CliPath $smoke
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & (Join-Path $root "scripts\verify-mcp-safety.ps1") -McpPath $mcpSmoke -ExpectedVersion $version
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & (Join-Path $root "scripts\verify-mcp-wire.ps1") -McpPath $mcpSmoke
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Remove-Item -Recurse -Force $extractCli -ErrorAction SilentlyContinue
    }

    $extractMain = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-pkg-main-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $extractMain | Out-Null
    try {
        Expand-Archive -Path $mainZip -DestinationPath $extractMain
        $mainSmoke = Join-Path $extractMain "spacelens.exe"
        $mainGui = Join-Path $extractMain "spacelens-gui.exe"
        $mainMcp = Join-Path $extractMain "spacelens-mcp.exe"
        if (-not (Test-Path $mainGui)) {
            Write-Error "unified zip missing spacelens-gui.exe"
        }
        if (-not (Test-Path $mainMcp)) {
            Write-Error "unified zip missing spacelens-mcp.exe"
        }
        if (-not (Test-Path (Join-Path $extractMain "platforms\qwindows.dll"))) {
            Write-Error "unified zip missing platforms\qwindows.dll"
        }
        $mainVer = & $mainSmoke version
        if ($mainVer -notmatch [regex]::Escape($version)) {
            Write-Error "Extracted unified CLI version '$mainVer' does not contain $version"
        }
        & (Join-Path $root "scripts\verify-cli-safety.ps1") -CliPath $mainSmoke
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & (Join-Path $root "scripts\verify-mcp-safety.ps1") -McpPath $mainMcp -ExpectedVersion $version
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & (Join-Path $root "scripts\verify-mcp-wire.ps1") -McpPath $mainMcp
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Remove-Item -Recurse -Force $extractMain -ErrorAction SilentlyContinue
    }
}

Write-Host "CLI archive: $cliZip"
Write-Host "Main archive: $mainZip"
Write-Host "Version: $version"
