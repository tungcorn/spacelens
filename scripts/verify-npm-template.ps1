# Offline checks for npm packaging templates. No native payload, no network.

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$failures = New-Object System.Collections.Generic.List[string]
function Add-Fail([string]$Message) {
    $script:failures.Add($Message)
    Write-Host "NPM TEMPLATE FAIL: $Message"
}

$pinPath = Join-Path $root "packaging\npm\release-pin.env"
$pkgPath = Join-Path $root "packaging\npm\package.json"
if (-not (Test-Path $pinPath)) { Add-Fail "missing release-pin.env" }
if (-not (Test-Path $pkgPath)) { Add-Fail "missing package.json" }

if ((Test-Path $pinPath) -and (Test-Path $pkgPath)) {
    $pin = @{}
    foreach ($line in Get-Content $pinPath) {
        $trim = $line.Trim()
        if (-not $trim -or $trim.StartsWith("#")) { continue }
        $eq = $trim.IndexOf("=")
        if ($eq -lt 1) { continue }
        $pin[$trim.Substring(0, $eq).Trim()] = $trim.Substring($eq + 1).Trim()
    }
    $pkg = Get-Content $pkgPath -Raw | ConvertFrom-Json
    if ($pkg.name -ne "@tungcorn/spacelens") { Add-Fail "package name" }
    if ($pkg.version -ne "0.1.1") { Add-Fail "package version must be 0.1.1" }
    if ($pkg.license -ne "MIT") { Add-Fail "package license must be MIT" }
    if ($pkg.os -notcontains "win32") { Add-Fail "os must be win32" }
    if ($pkg.cpu -notcontains "x64") { Add-Fail "cpu must be x64" }
    if (-not $pkg.bin.spacelens -or -not $pkg.bin.'spacelens-gui') {
        Add-Fail "bin must expose spacelens and spacelens-gui"
    }
    if ($pkg.scripts -and $pkg.scripts.postinstall) { Add-Fail "postinstall forbidden" }
    if ($pkg.dependencies) { Add-Fail "production dependencies forbidden" }
    if ($pkg.repository.url -notmatch "github.com/tungcorn/spacelens") {
        Add-Fail "repository.url must identify github.com/tungcorn/spacelens"
    }
    if ($pin.RELEASE_SHA256 -ne "b4d4cb993bb53e1414c9fc156d9c29a5dca1b8640ac8d3b1229e5ff5a345793d") {
        Add-Fail "release pin hash is not the published v0.1.1 unified zip"
    }
    if ($pin.SPACELENS_VERSION -ne "0.1.1") { Add-Fail "pin version" }
    if ($pin.RELEASE_ASSET -ne "spacelens-v0.1.1-windows-x64.zip") {
        Add-Fail "pin asset name"
    }
}

$binDir = Join-Path $root "packaging\npm\bin"
foreach ($name in @("launch.js", "spacelens.js", "spacelens-gui.js")) {
    $path = Join-Path $binDir $name
    if (-not (Test-Path $path)) {
        Add-Fail "missing $name"
        continue
    }
    $text = Get-Content $path -Raw
    if ($text -match "shell:\s*true") { Add-Fail "$name uses shell: true" }
    if ($name -eq "launch.js" -and $text -notmatch "shell:\s*false") {
        Add-Fail "launch.js must set shell: false"
    }
    if ($text -match "Invoke-WebRequest|https://github.com/.+/releases/download|curl |wget ") {
        Add-Fail "$name looks like a runtime downloader"
    }
    if ($text -match "LOCALAPPDATA|state\.db") {
        Add-Fail "$name must not touch AppData/state.db"
    }
}

$npmrc = Join-Path $root "packaging\npm\.npmrc"
if (Test-Path $npmrc) {
    $npmrcText = Get-Content $npmrc -Raw
    if ($npmrcText -match '(?i)(_auth|token|password)') {
        Add-Fail "packaging/npm/.npmrc must not contain credentials"
    }
}

$unit = Join-Path $root "tests\npm\test_launchers.js"
if (-not (Test-Path $unit)) {
    Add-Fail "missing tests/npm/test_launchers.js"
} else {
    node $unit
    if ($LASTEXITCODE -ne 0) { Add-Fail "launcher unit tests failed" }
}

if ($failures.Count -gt 0) {
    Write-Error "npm template verification failed ($($failures.Count) problem(s))"
}
Write-Host "npm template verification PASS"
exit 0
