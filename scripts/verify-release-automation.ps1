# Static checks for Release Automation V2 workflows. No publication.
# V2: push to main → prepare job bumps version directly on main → dispatch
# publish pipeline. No release/next PR.

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
$npm     = Get-Workflow ".github/workflows/npm-publish.yml"
$ci      = Get-Workflow ".github/workflows/ci.yml"

# ── release.yml structural invariants ───────────────────────────────────────
if ($release -notmatch 'workflow_dispatch:') { Add-Fail "release.yml must keep workflow_dispatch recovery" }
if ($release -notmatch 'push:') { Add-Fail "release.yml must run on push to main" }
if ($release -notmatch 'group:\s*spacelens-release') { Add-Fail "release.yml concurrency must be spacelens-release" }
if ($release -notmatch 'cancel-in-progress:\s*false') { Add-Fail "release publication must not cancel in-progress runs" }
if ($release -notmatch '(?s)workflow_dispatch:.*?version:.*?required:\s*true') {
    Add-Fail "release.yml version input must remain required"
}

# ── V2 prepare job ───────────────────────────────────────────────────────────
if ($release -notmatch 'name:\s*Prepare release on main') {
    Add-Fail "release.yml must contain 'Prepare release on main' job (V2 prepare)"
}
if ($release -notmatch 'prepare-release\.ps1') {
    Add-Fail "release.yml prepare job must call prepare-release.ps1"
}
if ($release -notmatch 'contents:\s*write') {
    Add-Fail "release.yml prepare job must have contents: write permission"
}
if ($release -notmatch 'actions:\s*write') {
    Add-Fail "release.yml prepare job must have actions: write permission (for dispatch)"
}

# Prepare flow must dispatch CI for the bump SHA, then publish — never the reverse.
$prepareSlice = $null
if ($release -match '(?s)name:\s*Prepare release on main(?<prep>.*)name:\s*Preflight') {
    $prepareSlice = $Matches['prep']
} else {
    Add-Fail "could not isolate the prepare job from release.yml"
}
if ($prepareSlice) {
    if ($prepareSlice -notmatch 'gh workflow run ci\.yml') {
        Add-Fail "prepare job must dispatch ci.yml for the bump commit"
    }
    if ($prepareSlice -notmatch 'gh workflow run release\.yml') {
        Add-Fail "prepare job must dispatch release.yml after CI"
    }
    $ciAt = $prepareSlice.IndexOf('gh workflow run ci.yml')
    $relAt = $prepareSlice.IndexOf('gh workflow run release.yml')
    if ($ciAt -lt 0 -or $relAt -lt 0 -or $ciAt -gt $relAt) {
        Add-Fail "prepare job must dispatch ci.yml before release.yml"
    }
    if ($prepareSlice -notmatch 'head_sha') {
        Add-Fail "prepare job must verify dispatched CI head_sha is the bump commit"
    }
    if ($prepareSlice -notmatch 'main moved before CI dispatch') {
        Add-Fail "prepare job must fail safely if main moved before CI dispatch"
    }
    if ($prepareSlice -notmatch 'main moved before publish dispatch') {
        Add-Fail "prepare job must fail safely if main moved before publish dispatch"
    }
}

# ── Safety invariants ────────────────────────────────────────────────────────
if ($release -match 'default:\s*"\d+\.\d+\.\d+"') {
    Add-Fail "release.yml must not hardcode a version-specific workflow_dispatch default"
}
if ($npm -match 'default:\s*"\d+\.\d+\.\d+"') {
    Add-Fail "npm-publish.yml must not hardcode a version-specific workflow_dispatch default"
}
if ($npm -notmatch '(?s)workflow_dispatch:.*?version:.*?required:\s*true') {
    Add-Fail "npm-publish.yml version input must remain required"
}
if ($npm -notmatch 'id-token:\s*write') { Add-Fail "npm-publish.yml must keep OIDC id-token: write" }
if ($npm -match 'NPM_TOKEN') { Add-Fail "npm-publish.yml must not mention NPM_TOKEN" }
if ($release -match 'NPM_TOKEN') { Add-Fail "release.yml must not mention NPM_TOKEN" }
if ($ci -notmatch 'Release / Policy') { Add-Fail "ci.yml must run Release / Policy" }

# Preflight must wait for the six required checks on the exact bump SHA.
$preflight = Get-Content (Join-Path $root "scripts\verify-release-preflight.ps1") -Raw
foreach ($check in @(
    "Windows / Full Debug",
    "Windows / Full Release",
    "Windows / CLI-only Latest",
    "Quality / MSVC Analyze",
    "npm / Package",
    "Release / Policy"
)) {
    if ($preflight -notmatch [regex]::Escape($check)) {
        Add-Fail "preflight must wait for required CI check '$check'"
    }
}
if ($preflight -notmatch 'commits/\$Commit/check-runs' -and $preflight -notmatch 'commits/\$\{Sha\}/check-runs' -and $preflight -notmatch 'commits/\$Sha/check-runs') {
    Add-Fail "preflight must query check-runs on the exact commit SHA"
}

# ── pin job must dispatch ci.yml (no longer dispatches release-pr.yml) ───────
if ($release -notmatch 'gh workflow run ci\.yml') {
    Add-Fail "pin job must dispatch ci.yml after pin commit"
}
if ($release -match 'gh workflow run release-pr\.yml') {
    Add-Fail "pin job must not dispatch release-pr.yml (retired in V2)"
}

# ── npm publish poll guard ───────────────────────────────────────────────────
if ($release -match 'select\(\.head_sha == \$sha\)') {
    Add-Fail "npm-publish poll must not require head_sha == GITHUB_SHA (main may have moved)"
}

# ── release-please guard ─────────────────────────────────────────────────────
if ($release -match 'release-please') {
    Add-Fail "Release Please must not own tag or GitHub Release"
}

# ── mechanical version files must not include workflow files ─────────────────
$mechFiles = @(Get-SpaceLensMechanicalVersionFiles)
if ($mechFiles -notcontains "docs/QT_SOURCE_OFFER.md") {
    Add-Fail "mechanical version files must include docs/QT_SOURCE_OFFER.md"
}
foreach ($rel in $mechFiles) {
    if ($rel -match '(?i)(?:^|/)\.github/workflows/') {
        Add-Fail "mechanical version files must not include $rel"
    }
}

# ── WinGet guard ─────────────────────────────────────────────────────────────
if ($release -match 'winget') {
    Add-Fail "release automation must not mention WinGet"
}

# ── CMake / package.json version sync ────────────────────────────────────────
$cmake = Get-SpaceLensCMakeVersion -RepoRoot $root
$pkg   = Get-SpaceLensNpmVersion   -RepoRoot $root
if ($cmake.Text -ne $pkg.Text) {
    Add-Fail "CMake $($cmake.Text) != package.json $($pkg.Text)"
}

# ── Required script files ────────────────────────────────────────────────────
foreach ($rel in @(
    "scripts/SpaceLensRelease.ps1",
    "scripts/release-needed.ps1",
    "scripts/prepare-release.ps1",
    "scripts/decide-release.ps1",
    "scripts/update-release-pin.ps1",
    "scripts/verify-release-policy.ps1",
    "scripts/verify-release-automation.ps1"
)) {
    $path = Join-Path $root ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $path)) { Add-Fail "missing $rel" }
}

# ── Pinned action SHA check (release.yml + npm-publish.yml + ci.yml) ─────────
foreach ($rel in @(
    ".github/workflows/release.yml",
    ".github/workflows/npm-publish.yml",
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

# ── Dry-run current tree ──────────────────────────────────────────────────────
& (Join-Path $PSScriptRoot "release-needed.ps1") -DryRun | Out-Host
& (Join-Path $PSScriptRoot "decide-release.ps1") | Out-Host
$prepDry = & (Join-Path $PSScriptRoot "prepare-release.ps1") -DryRun | Out-String
if ($LASTEXITCODE -ne 0) {
    Add-Fail "prepare-release.ps1 -DryRun failed"
} elseif ($prepDry -match '\.github/workflows/') {
    Add-Fail "prepare-release dry-run must not list .github/workflows files"
}

if ($failures.Count -gt 0) {
    Write-Host "verify-release-automation: $($failures.Count) failure(s)"
    exit 1
}
Write-Host "verify-release-automation: PASS"
exit 0
