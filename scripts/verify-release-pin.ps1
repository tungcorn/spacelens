# Offline format/consistency check of packaging/npm/release-pin.env.
# Does not download the public zip. Publication already proved that hash.

[CmdletBinding()]
param([string]$RepoRoot = "")

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

if (-not $RepoRoot) { $RepoRoot = Get-SpaceLensRepoRoot }

$pinPath = Join-Path $RepoRoot "packaging\npm\release-pin.env"
if (-not (Test-Path -LiteralPath $pinPath)) { throw "missing packaging/npm/release-pin.env" }

$pin = @{}
foreach ($line in Get-Content -LiteralPath $pinPath) {
    $trim = $line.Trim()
    if (-not $trim -or $trim.StartsWith("#")) { continue }
    $eq = $trim.IndexOf("=")
    if ($eq -lt 1) { continue }
    $pin[$trim.Substring(0, $eq).Trim()] = $trim.Substring($eq + 1).Trim()
}

foreach ($key in @("SPACELENS_VERSION", "RELEASE_TAG", "RELEASE_ASSET", "RELEASE_SHA256", "RELEASE_URL")) {
    if (-not $pin[$key]) { throw "release-pin.env missing $key" }
}

$v = ConvertTo-SpaceLensVersion $pin.SPACELENS_VERSION
if (-not $v) { throw "SPACELENS_VERSION '$($pin.SPACELENS_VERSION)' is not X.Y.Z" }
if ($pin.RELEASE_TAG -ne $v.Tag) { throw "RELEASE_TAG $($pin.RELEASE_TAG) != $($v.Tag)" }
if ($pin.RELEASE_ASSET -ne "spacelens-$($v.Tag)-windows-x64.zip") {
    throw "RELEASE_ASSET $($pin.RELEASE_ASSET) does not match $($v.Tag)"
}
if ($pin.RELEASE_SHA256 -notmatch '^[0-9a-f]{64}$') {
    throw "RELEASE_SHA256 is not a lowercase 64-char hex digest"
}
if ($pin.RELEASE_URL -notmatch [regex]::Escape($pin.RELEASE_ASSET)) {
    throw "RELEASE_URL does not name RELEASE_ASSET"
}

$cmake = Get-SpaceLensCMakeVersion -RepoRoot $RepoRoot
$npm = Get-SpaceLensNpmVersion -RepoRoot $RepoRoot
if ($cmake.Text -ne $npm.Text) {
    throw "CMake $($cmake.Text) != package.json $($npm.Text)"
}
if ($cmake.Text -eq $v.Text) {
    Write-Host "pin version matches CMake/package.json $($v.Text)"
} else {
    Write-Host "package $($cmake.Text) is ahead of pin $($v.Text); format still valid"
}

Write-Host "verify-release-pin: PASS"
exit 0
