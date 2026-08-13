# Sets up MSVC + Windows SDK + Qt for SpaceLens builds.
# Usage:  . .\scripts\dev-env.ps1
#
# Qt is discovered via CMAKE_PREFIX_PATH or well-known prefixes. Do not put
# machine-specific Qt paths in CMakePresets.json.

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "use-msvc.ps1")
$script:spacelensMsvcFromVcvars = Import-SpaceLensMsvc

function Find-MsvcRoot {
    $candidates = @(
        "D:\visual-studio-2026\VC\Tools\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
    )
    foreach ($root in $candidates) {
        if (Test-Path $root) {
            $ver = Get-ChildItem $root -Directory | Sort-Object Name -Descending | Select-Object -First 1
            if ($ver) { return $ver.FullName }
        }
    }
    return $null
}

function Find-WindowsKit {
    $kitRoot = "C:\Program Files (x86)\Windows Kits\10"
    if (-not (Test-Path "$kitRoot\Include")) {
        $kitRoot = "C:\Program Files\Windows Kits\10"
    }
    if (-not (Test-Path "$kitRoot\Include")) {
        return $null
    }
    $ver = Get-ChildItem "$kitRoot\Include" -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName "um\Windows.h") } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if (-not $ver) { return $null }
    return [pscustomobject]@{
        Root = $kitRoot
        Version = $ver.Name
    }
}

function Find-QtPrefix {
    if ($env:CMAKE_PREFIX_PATH) {
        foreach ($entry in ($env:CMAKE_PREFIX_PATH -split ";")) {
            if ($entry -and (Test-Path (Join-Path $entry "lib\cmake\Qt6\Qt6Config.cmake"))) {
                return $entry
            }
        }
    }
    $candidates = @(
        "D:\Qt\6.8.3\msvc2022_64",
        "D:\Qt\6.8.2\msvc2022_64",
        "D:\Qt\6.7.3\msvc2022_64",
        "C:\Qt\6.8.3\msvc2022_64"
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "lib\cmake\Qt6\Qt6Config.cmake")) {
            return $c
        }
    }
    return $null
}

$kit = Find-WindowsKit
$msvc = $null

if (-not $script:spacelensMsvcFromVcvars) {
    $msvc = Find-MsvcRoot
    if (-not $msvc) {
        Write-Error "MSVC toolset not found. Install Visual Studio C++ tools."
    }
    if (-not $kit) {
        Write-Error "Windows SDK with Windows.h not found."
    }

    $env:PATH = "$msvc\bin\Hostx64\x64;$($kit.Root)\bin\$($kit.Version)\x64;" + $env:PATH
    $env:INCLUDE = @(
        "$msvc\include",
        "$($kit.Root)\Include\$($kit.Version)\ucrt",
        "$($kit.Root)\Include\$($kit.Version)\um",
        "$($kit.Root)\Include\$($kit.Version)\shared",
        "$($kit.Root)\Include\$($kit.Version)\winrt"
    ) -join ";"
    $env:LIB = @(
        "$msvc\lib\x64",
        "$($kit.Root)\Lib\$($kit.Version)\ucrt\x64",
        "$($kit.Root)\Lib\$($kit.Version)\um\x64"
    ) -join ";"
}

$qt = Find-QtPrefix
if ($qt) {
    $env:CMAKE_PREFIX_PATH = $qt
    if ($env:PATH -notlike "*$qt\bin*") {
        $env:PATH = "$qt\bin;" + $env:PATH
    }
}

Write-Host "SpaceLens dev environment"
if ($msvc) {
    Write-Host "  MSVC:    $msvc"
} else {
    Write-Host "  MSVC:    (vcvars64)"
}
if ($kit) {
    Write-Host "  WinSDK:  $($kit.Root) ($($kit.Version))"
    Write-Host "  Windows.h: $(Test-Path "$($kit.Root)\Include\$($kit.Version)\um\Windows.h")"
}
Write-Host "  cl:      $((Get-Command cl -ErrorAction SilentlyContinue).Source)"
Write-Host "  Qt:      $(if ($qt) { $qt } else { '(not found — set CMAKE_PREFIX_PATH)' })"
Write-Host "  CMAKE_PREFIX_PATH=$env:CMAKE_PREFIX_PATH"
