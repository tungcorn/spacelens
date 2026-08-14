# Launch the locally built SpaceLens GUI.
# Never publishes, packages npm, downloads a GitHub Release, or touches
# the user-installed copy.
#
# Usage (from repo root):
#   .\scripts\dev.ps1
#   .\scripts\dev.ps1 -Config release

[CmdletBinding()]
param(
    [ValidateSet("debug", "release")]
    [string]$Config = "debug"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

. (Join-Path $PSScriptRoot "dev-env.ps1")

$preset = if ($Config -eq "release") { "windows-release" } else { "windows-debug" }
$buildDir = Join-Path $root $(if ($Config -eq "release") { "build-release" } else { "build-debug" })
$cache = Join-Path $buildDir "CMakeCache.txt"

if (-not (Test-Path $cache)) {
    Write-Host "Configuring $preset..."
    cmake --preset $preset
}

Write-Host "Building SpaceLens ($preset)..."
cmake --build --preset $preset --target SpaceLens
if ($LASTEXITCODE -ne 0) {
    Write-Error "GUI build failed."
}

$exe = Join-Path $buildDir "gui\spacelens-gui.exe"
if (-not (Test-Path $exe)) {
    Write-Error "Local GUI not found: $exe"
}

function Find-QtBin {
    param([string]$CachePath)
    $lines = Get-Content -LiteralPath $CachePath
    foreach ($line in $lines) {
        if ($line -match '^CMAKE_PREFIX_PATH:(STRING|PATH)=(.+)$') {
            foreach ($entry in ($Matches[2] -split ';')) {
                $bin = Join-Path $entry "bin"
                if (Test-Path (Join-Path $bin "Qt6Core.dll")) {
                    return $bin
                }
            }
        }
        if ($line -match '^Qt6_DIR:PATH=(.+)$') {
            $candidate = [System.IO.Path]::GetFullPath((Join-Path $Matches[1] "..\..\..\bin"))
            if (Test-Path (Join-Path $candidate "Qt6Core.dll")) {
                return $candidate
            }
        }
    }
    if ($env:CMAKE_PREFIX_PATH) {
        foreach ($entry in ($env:CMAKE_PREFIX_PATH -split ';')) {
            $bin = Join-Path $entry "bin"
            if (Test-Path (Join-Path $bin "Qt6Core.dll")) {
                return $bin
            }
        }
    }
    return $null
}

$qtBin = Find-QtBin -CachePath $cache
if ($qtBin) {
    $env:PATH = "$qtBin;$env:PATH"
}

Write-Host "Launching $exe"
Start-Process -FilePath $exe
