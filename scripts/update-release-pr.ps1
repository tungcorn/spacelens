# Create or update the single pending release/next PR. Does not tag or publish.

[CmdletBinding()]
param(
    [switch]$DryRun,
    [string]$RepoRoot = "",
    [string]$Repository = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

if (-not $RepoRoot) { $RepoRoot = Get-SpaceLensRepoRoot }
if (-not $Repository) {
    $Repository = if ($env:GITHUB_REPOSITORY) { $env:GITHUB_REPOSITORY } else { "tungcorn/spacelens" }
}

$branch = "release/next"

# Classify origin/main, not a possibly rewritten local branch.
& git -C $RepoRoot fetch origin main --tags --force
if ($LASTEXITCODE -ne 0) { throw "git fetch origin main failed" }

$latest = Get-SpaceLensLatestStableVersion -Tags (Get-SpaceLensGitTags -RepoRoot $RepoRoot)
if (-not $latest) { throw "no stable vX.Y.Z tag found" }

$cmakeMainRaw = (& git -C $RepoRoot show "origin/main:CMakeLists.txt" | Out-String)
if ($LASTEXITCODE -ne 0) { throw "cannot read origin/main CMakeLists.txt" }
if ($cmakeMainRaw -notmatch 'project\(\s*SpaceLens\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "origin/main CMake project VERSION not found"
}
$cmakeMain = ConvertTo-SpaceLensVersion $Matches[1]
$commits = Get-SpaceLensCommitsSinceTag -Tag $latest.Tag -Head "origin/main" -RepoRoot $RepoRoot
$decision = Get-SpaceLensReleaseDecision -Commits $commits -LatestVersion $latest
$next = Get-SpaceLensNextPatchVersion $latest

Write-Host (Format-SpaceLensDryRun -Decision $decision -Head "origin/main")
Write-Host "main_cmake=$($cmakeMain.Text)"

$openJson = & gh pr list --repo $Repository --base main --head $branch --state open --json number,title,url
if ($LASTEXITCODE -ne 0) { throw "gh pr list failed" }
$open = @()
if ($openJson) {
    $parsed = $openJson | ConvertFrom-Json
    if ($parsed) { $open = @($parsed) }
}
if ($open.Count -gt 1) {
    throw "more than one open $branch PR; refuse to create another"
}

function Close-PendingReleasePr {
    param($Items, [string]$Comment)
    foreach ($pr in $Items) {
        Write-Host "closing PR #$($pr.number) ($($pr.title))"
        if ($script:DryRun) { continue }
        & gh pr close $pr.number --repo $script:Repository --comment $Comment
        if ($LASTEXITCODE -ne 0) { throw "failed to close PR #$($pr.number)" }
    }
}

if ($cmakeMain.Text -eq $next.Text) {
    Write-Host "action=noop_already_prepared"
    if ($open.Count -gt 0) {
        Close-PendingReleasePr -Items $open -Comment "main already has $($next.Text); the pending release PR is no longer needed."
    }
    exit 0
}

if (-not $decision.Needed) {
    Write-Host "action=noop_not_releasable"
    if ($open.Count -gt 0) {
        Close-PendingReleasePr -Items $open -Comment "No releasable commits since $($latest.Tag). Closing the pending release PR."
    }
    exit 0
}

$title = "chore(release): prepare SpaceLens $($next.Tag)"
$notesRel = "docs/release-notes/$($next.Tag).md"
Write-Host "action=upsert"
Write-Host "pr_title=$title"

if ($DryRun) { exit 0 }

$savedNotes = $null
$remoteHeads = & git -C $RepoRoot ls-remote --heads origin $branch
$remoteExists = [bool]("$remoteHeads".Trim())
if ($remoteExists) {
    & git -C $RepoRoot fetch origin $branch
    if ($LASTEXITCODE -ne 0) { throw "git fetch origin $branch failed" }
    $blob = & git -C $RepoRoot show "origin/${branch}:$notesRel" 2>$null
    if ($LASTEXITCODE -eq 0 -and $blob) {
        $savedNotes = ($blob -join "`n")
        Write-Host "preserved $notesRel from origin/$branch"
    }
}

& git -C $RepoRoot checkout -B $branch origin/main
if ($LASTEXITCODE -ne 0) { throw "checkout $branch from origin/main failed" }
& git -C $RepoRoot reset --hard origin/main
if ($LASTEXITCODE -ne 0) { throw "reset --hard origin/main failed" }

if ($savedNotes) {
    $notesPath = Join-Path $RepoRoot ($notesRel -replace '/', [IO.Path]::DirectorySeparatorChar)
    $notesDir = Split-Path -Parent $notesPath
    if (-not (Test-Path -LiteralPath $notesDir)) {
        New-Item -ItemType Directory -Path $notesDir | Out-Null
    }
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($notesPath, $savedNotes.TrimEnd() + "`n", $utf8)
}

& (Join-Path $PSScriptRoot "prepare-release.ps1") -Version $next.Text -RepoRoot $RepoRoot
if ($LASTEXITCODE -ne 0) { throw "prepare-release.ps1 failed" }

$toAdd = @(Get-SpaceLensMechanicalVersionFiles) + @("CHANGELOG.md", $notesRel)
foreach ($rel in $toAdd) {
    $path = Join-Path $RepoRoot ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (Test-Path -LiteralPath $path) {
        & git -C $RepoRoot add -- $rel
        if ($LASTEXITCODE -ne 0) { throw "git add $rel failed" }
    }
}

$porcelain = & git -C $RepoRoot status --porcelain
if (-not "$porcelain".Trim()) {
    throw "prepare-release produced no changes for $($next.Text)"
}

& git -C $RepoRoot config user.name "github-actions[bot]"
& git -C $RepoRoot config user.email "41898282+github-actions[bot]@users.noreply.github.com"
& git -C $RepoRoot commit -m "chore(release): prepare SpaceLens $($next.Tag)"
if ($LASTEXITCODE -ne 0) { throw "git commit failed" }

if ($remoteExists) {
    $lease = (& git -C $RepoRoot rev-parse "origin/$branch").Trim()
    & git -C $RepoRoot push --force-with-lease="refs/heads/${branch}:${lease}" origin $branch
} else {
    & git -C $RepoRoot push -u origin $branch
}
if ($LASTEXITCODE -ne 0) { throw "git push $branch failed" }

if ($open.Count -eq 0) {
    $body = @"
This is the single pending SpaceLens **patch** release.

Merging this pull request is the publish approval. After it lands on ``main``, ``release.yml`` waits for required CI, creates annotated tag ``$($next.Tag)`` at that commit, publishes the GitHub Release, runs npm Trusted Publishing, then pins ``release-pin.env`` from the **public** unified zip.

- Do not merge if you do not want to publish ``$($next.Tag)``.
- ``docs:`` / ``test:`` / ``ci:`` / ``chore:`` / ``style:`` / ``build:`` commits do not open a release by themselves unless they also touch ``src/core``, ``src/cli``, ``src/mcp``, ``src/app``, ``src/ui``, or ``src/platform``.
- Generated notes sit between ``<!-- BEGIN GENERATED NOTES -->`` markers. Edits outside those markers are preserved on the next refresh.
- Release Automation V1 prepares the next patch only. It does not infer major or minor versions.

Manual recovery remains ``workflow_dispatch`` on ``release.yml`` / ``npm-publish.yml``.
"@
    $bodyFile = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-pr-" + [guid]::NewGuid().ToString("N") + ".md")
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($bodyFile, $body, $utf8)
    try {
        & gh pr create --repo $Repository --base main --head $branch --title $title --body-file $bodyFile
        if ($LASTEXITCODE -ne 0) { throw "gh pr create failed" }
    } finally {
        Remove-Item -LiteralPath $bodyFile -Force -ErrorAction SilentlyContinue
    }
} elseif ($open[0].title -ne $title) {
    & gh pr edit $open[0].number --repo $Repository --title $title
    if ($LASTEXITCODE -ne 0) { throw "gh pr edit failed" }
}

Write-Host "updated $branch for $($next.Tag)"
exit 0
