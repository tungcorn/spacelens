# Install a pinned Qt MSVC x64 kit with aqtinstall.
# Used by CI. Local developers may use an existing Qt prefix instead.
#
# Pins:
#   Qt        6.8.3 win64_msvc2022_64
#   aqtinstall 3.3.0
#
# Success stdout is exactly one line: the Qt prefix. Everything else is
# Write-Host so `$prefix = & .\scripts\install-qt.ps1` is a real path.

[CmdletBinding()]
param(
    [string]$Version = "6.8.3",
    [string]$Arch = "win64_msvc2022_64",
    [string]$AqtVersion = "3.3.0",
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"

function Invoke-LoggedNative {
    param([Parameter(Mandatory = $true)][string[]]$NativeArgs)
    $output = & $NativeArgs[0] @($NativeArgs[1..($NativeArgs.Length - 1)]) 2>&1
    $code = $LASTEXITCODE
    foreach ($line in $output) { Write-Host $line }
    if ($code -ne 0) {
        throw "Command failed with exit $code : $($NativeArgs -join ' ')"
    }
}

if (-not $Output) {
    $Output = Join-Path $env:RUNNER_TEMP "Qt"
    if (-not $env:RUNNER_TEMP) {
        $Output = Join-Path ([System.IO.Path]::GetTempPath()) "spacelens-qt"
    }
}

$prefix = Join-Path $Output "$Version\msvc2022_64"
$config = Join-Path $prefix "lib\cmake\Qt6\Qt6Config.cmake"
if (Test-Path $config) {
    Write-Host "Qt $Version already present at $prefix"
    $env:CMAKE_PREFIX_PATH = $prefix
    Write-Host "CMAKE_PREFIX_PATH=$prefix"
    Write-Output $prefix
    return
}

Invoke-LoggedNative @(
    "python", "-m", "pip", "install", "--disable-pip-version-check", "aqtinstall==$AqtVersion"
)
New-Item -ItemType Directory -Force -Path $Output | Out-Null
Invoke-LoggedNative @(
    "python", "-m", "aqt", "install-qt", "windows", "desktop", $Version, $Arch, "-O", $Output
)

if (-not (Test-Path $config)) {
    Write-Error "Qt $Version installed but Qt6Config.cmake was not found at $config"
}

$env:CMAKE_PREFIX_PATH = $prefix
Write-Host "CMAKE_PREFIX_PATH=$prefix"
Write-Output $prefix
