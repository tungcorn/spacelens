# Static checks for Release Automation V1 workflows. No publication.

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

$root = Get-SpaceLensRepoRoot
$failures = New-Object System.Collections.Generic.List[string]
function Add-Fail([string]$Message) {
    $script:failures.Add($Message)
    Write-Host "AUTOMATION FAIL: $Message"
}

function Get-Workflow([string]$Rel) {
    $path = Join-Path $root ($Rel -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $path)) { throw "missing $Rel" }
    return Get-Content -LiteralPath $path -Raw
}

$release = Get-Workflow ".github/workflows/release.yml"
$npm = Get-Workflow ".github/workflows/npm-publish.yml"
$pr = Get-Workflow ".github/workflows/release-pr.yml"
$ci = Get-Workflow ".github/workflows/ci.yml"

if ($release -notmatch 'workflow_dispatch:') { Add-Fail "release.yml must keep workflow_dispatch recovery" }
if ($release -notmatch 'push:') { Add-Fail "release.yml must run on push to main" }
if ($release -notmatch 'group:\s*spacelens-release') { Add-Fail "release.yml concurrency must be spacelens-release" }
if ($release -notmatch 'cancel-in-progress:\s*false') { Add-Fail "release publication must not cancel in-progress runs" }
if ($npm -notmatch 'id-token:\s*write') { Add-Fail "npm-publish.yml must keep OIDC id-token: write" }
if ($npm -match 'NPM_TOKEN') { Add-Fail "npm-publish.yml must not mention NPM_TOKEN" }
if ($release -match 'NPM_TOKEN') { Add-Fail "release.yml must not mention NPM_TOKEN" }
if ($pr -match 'NPM_TOKEN') { Add-Fail "release-pr.yml must not mention NPM_TOKEN" }
if ($pr -notmatch 'group:\s*spacelens-release-pr') { Add-Fail "release-pr.yml concurrency group missing" }
if ($pr -notmatch 'pull-requests:\s*write') { Add-Fail "release-pr.yml needs pull-requests: write on the updater job" }
if ($ci -notmatch 'Release / Policy') { Add-Fail "ci.yml must run Release / Policy" }

foreach ($rel in @(
    ".github/workflows/release.yml",
    ".github/workflows/npm-publish.yml",
    ".github/workflows/release-pr.yml",
    ".github/workflows/ci.yml"
)) {
    $text = Get-Workflow $rel
    $uses = [regex]::Matches($text, '(?m)^\s+uses:\s+(\S+)')
    foreach ($use in $uses) {
        $action = $use.Groups[1].Value
        if ($action -match '^actions/' -and $action -notmatch '@[0-9a-f]{40}') {
            Add-Fail "$rel uses unpinned action $action"
        }
        if ($action -match '@(main|master|v\d+)') {
            Add-Fail "$rel uses floating action ref $action"
        }
    }
}

if ($release -match 'winget' -or $pr -match 'winget') {
    Add-Fail "release automation must not mention WinGet"
}

$cmake = Get-SpaceLensCMakeVersion -RepoRoot $root
$pkg = Get-SpaceLensNpmVersion -RepoRoot $root
if ($cmake.Text -ne $pkg.Text) {
    Add-Fail "CMake $($cmake.Text) != package.json $($pkg.Text)"
}

foreach ($rel in @(
    "scripts/SpaceLensRelease.ps1",
    "scripts/release-needed.ps1",
    "scripts/prepare-release.ps1",
    "scripts/decide-release.ps1",
    "scripts/update-release-pr.ps1",
    "scripts/update-release-pin.ps1",
    "scripts/verify-release-policy.ps1",
    "scripts/verify-release-automation.ps1"
)) {
    $path = Join-Path $root ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $path)) { Add-Fail "missing $rel" }
}

if ($release -match 'release-please' -or $pr -match 'release-please') {
    Add-Fail "Release Please must not own tag or GitHub Release"
}

if ($release -notmatch 'gh workflow run ci.yml') {
    Add-Fail "pin job must dispatch ci.yml"
}
if ($release -notmatch 'gh workflow run release-pr.yml') {
    Add-Fail "pin job must dispatch release-pr.yml so later product commits get a pending PR"
}
if ($release -match 'select\(\.head_sha == \$sha\)') {
    Add-Fail "npm-publish poll must not require head_sha == GITHUB_SHA (main may have moved)"
}

# Dry-run the current tree (may or may not be releasable after this milestone).
& (Join-Path $PSScriptRoot "release-needed.ps1") -DryRun | Out-Host
& (Join-Path $PSScriptRoot "decide-release.ps1") | Out-Host

if ($failures.Count -gt 0) {
    Write-Host "verify-release-automation: $($failures.Count) failure(s)"
    exit 1
}
Write-Host "verify-release-automation: PASS"
exit 0
