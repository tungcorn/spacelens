# Stage portable Windows x64 CLI and GUI zip archives from a Release build.
# Uses cmake --install components, then windeployqt for the GUI.

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

function New-Sha256Sums([string]$Directory) {
    $out = Join-Path $Directory "SHA256SUMS.txt"
    $lines = @()
    Get-ChildItem $Directory -File -Filter "*.zip" | Sort-Object Name | ForEach-Object {
        $hash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant()
        $lines += "$hash  $($_.Name)"
    }
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllLines($out, $lines, $utf8)
    Write-Host "Wrote $out"
    return $out
}

$version = Get-ProjectVersion (Join-Path $BuildDir "CMakeCache.txt")
$cliName = "spacelens-cli-v$version-windows-x64"
$guiName = "spacelens-gui-v$version-windows-x64"

$stage = Join-Path $root "stage"
$cliStage = Join-Path $stage $cliName
$guiStage = Join-Path $stage $guiName
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $cliStage, $guiStage, $OutDir | Out-Null

function Copy-IfLicense([string]$Destination) {
    $license = Join-Path $root "LICENSE"
    if (Test-Path $license) {
        $text = (Get-Content $license -Raw -ErrorAction SilentlyContinue)
        if ($text -and $text.Trim().Length -gt 0) {
            Copy-Item $license (Join-Path $Destination "LICENSE")
        }
    }
}

Write-Host "Checking Qt review record (well-formed, not RequirePass)"
& (Join-Path $root "scripts\verify-qt-redist-review.ps1")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Installing CLI component -> $cliStage"
cmake --install $BuildDir --prefix $cliStage --component SpaceLensCli
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item (Join-Path $root "packaging\cli\README.txt") (Join-Path $cliStage "README.txt")
Copy-Item (Join-Path $root "packaging\cli\THIRD_PARTY_NOTICES.txt") (Join-Path $cliStage "THIRD_PARTY_NOTICES.txt")
Copy-IfLicense $cliStage

$cliExe = Join-Path $cliStage "spacelens.exe"
if (-not (Test-Path $cliExe)) {
    Write-Error "CLI install did not produce spacelens.exe"
}

Write-Host "Installing GUI component -> $guiStage"
cmake --install $BuildDir --prefix $guiStage --component SpaceLensGui
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item (Join-Path $root "packaging\gui\README.txt") (Join-Path $guiStage "README.txt")
Copy-Item (Join-Path $root "packaging\gui\THIRD_PARTY_NOTICES.txt") (Join-Path $guiStage "THIRD_PARTY_NOTICES.txt")
$guiLicenses = Join-Path $guiStage "licenses"
New-Item -ItemType Directory -Force -Path $guiLicenses | Out-Null
Copy-Item (Join-Path $root "packaging\gui\licenses\*") $guiLicenses -Force
Copy-IfLicense $guiStage

$guiExe = Join-Path $guiStage "spacelens-gui.exe"
if (-not (Test-Path $guiExe)) {
    Write-Error "GUI install did not produce spacelens-gui.exe"
}

$windeploy = Get-Windeployqt
Write-Host "Deploying Qt runtime with $windeploy"
& $windeploy --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --release --dir $guiStage $guiExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Inventory deployed Qt modules for the third-party audit.
$qtDlls = Get-ChildItem $guiStage -Filter "Qt6*.dll" | Sort-Object Name | ForEach-Object { $_.Name }
Write-Host "Deployed Qt DLLs: $($qtDlls -join ', ')"

& (Join-Path $root "scripts\verify-package.ps1") -CliStage $cliStage -GuiStage $guiStage
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$cliZip = Join-Path $OutDir "$cliName.zip"
$guiZip = Join-Path $OutDir "$guiName.zip"
if (Test-Path $cliZip) { Remove-Item -Force $cliZip }
if (Test-Path $guiZip) { Remove-Item -Force $guiZip }

Compress-Archive -Path (Join-Path $cliStage "*") -DestinationPath $cliZip
Compress-Archive -Path (Join-Path $guiStage "*") -DestinationPath $guiZip
New-Sha256Sums $OutDir | Out-Null

if (-not $SkipSmoke) {
    $extract = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-pkg-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $extract | Out-Null
    try {
        Expand-Archive -Path $cliZip -DestinationPath $extract
        $smoke = Join-Path $extract "spacelens.exe"
        $verOut = & $smoke version
        if ($verOut -notmatch [regex]::Escape($version)) {
            Write-Error "Extracted CLI version '$verOut' does not contain $version"
        }
        & (Join-Path $root "scripts\verify-cli-safety.ps1") -CliPath $smoke
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Remove-Item -Recurse -Force $extract -ErrorAction SilentlyContinue
    }
}

Write-Host "CLI archive: $cliZip"
Write-Host "GUI archive: $guiZip"
Write-Host "Version: $version"
