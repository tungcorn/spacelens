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
    $cmakeText = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
    $cmakeVersion = $null
    if ($cmakeText -match 'project\(\s*SpaceLens\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        $cmakeVersion = $Matches[1]
    }
    if (-not $cmakeVersion) { Add-Fail "CMake project VERSION not found" }
    if ($pkg.name -ne "@tungcorn/spacelens") { Add-Fail "package name" }
    if ($cmakeVersion -and $pkg.version -ne $cmakeVersion) {
        Add-Fail "package.json version '$($pkg.version)' != CMake $cmakeVersion"
    }
    if ($pkg.license -ne "MIT") { Add-Fail "package license must be MIT" }
    if ($pkg.os -notcontains "win32") { Add-Fail "os must be win32" }
    if ($pkg.cpu -notcontains "x64") { Add-Fail "cpu must be x64" }
    if (-not $pkg.bin.spacelens -or -not $pkg.bin.'spacelens-gui' -or -not $pkg.bin.'spacelens-mcp') {
        Add-Fail "bin must expose spacelens, spacelens-gui, and spacelens-mcp"
    }
    if ($pkg.scripts -and $pkg.scripts.postinstall) { Add-Fail "postinstall forbidden" }
    if ($pkg.dependencies) { Add-Fail "production dependencies forbidden" }
    if ($pkg.repository.url -notmatch "github.com/tungcorn/spacelens") {
        Add-Fail "repository.url must identify github.com/tungcorn/spacelens"
    }
    if ($pin.RELEASE_SHA256 -notmatch '^[0-9a-f]{64}$') {
        Add-Fail "release pin hash must be 64-char lowercase hex"
    }
    if ($pin.SPACELENS_VERSION -eq "0.1.1" -and $pin.RELEASE_SHA256 -ne "b4d4cb993bb53e1414c9fc156d9c29a5dca1b8640ac8d3b1229e5ff5a345793d") {
        Add-Fail "v0.1.1 pin hash is not the published unified zip"
    }
    $expectedAsset = "spacelens-v$($pin.SPACELENS_VERSION)-windows-x64.zip"
    if ($pin.RELEASE_ASSET -ne $expectedAsset) {
        Add-Fail "pin asset name must be $expectedAsset"
    }
    if ($pin.SPACELENS_VERSION -eq $pkg.version -and $pin.RELEASE_TAG -ne "v$($pkg.version)") {
        Add-Fail "pin tag must match current package version"
    }
}

$binDir = Join-Path $root "packaging\npm\bin"
foreach ($name in @("launch.js", "spacelens.js", "spacelens-gui.js", "spacelens-mcp.js")) {
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
