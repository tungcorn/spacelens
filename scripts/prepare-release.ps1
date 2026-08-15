# Prepare mechanical version files, CHANGELOG, and release notes for the
# next patch. Does not tag, publish, or update release-pin.env.

[CmdletBinding()]
param(
    [string]$Version = "",
    [switch]$DryRun,
    [string]$Date = "",
    [string]$Head = "HEAD",
    [string]$RepoRoot = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

if (-not $RepoRoot) { $RepoRoot = Get-SpaceLensRepoRoot }
if (-not $Date) { $Date = [datetime]::UtcNow.ToString("yyyy-MM-dd") }

$cmake = Get-SpaceLensCMakeVersion -RepoRoot $RepoRoot
$npm = Get-SpaceLensNpmVersion -RepoRoot $RepoRoot
if ($cmake.Text -ne $npm.Text) {
    throw "CMake $($cmake.Text) != package.json $($npm.Text)"
}

$tags = Get-SpaceLensGitTags -RepoRoot $RepoRoot
$latest = Get-SpaceLensLatestStableVersion -Tags $tags
if (-not $latest) { throw "no stable vX.Y.Z tag found" }

$commits = Get-SpaceLensCommitsSinceTag -Tag $latest.Tag -Head $Head -RepoRoot $RepoRoot
$decision = Get-SpaceLensReleaseDecision -Commits $commits -LatestVersion $latest

$target = $null
if ($Version) {
    $target = ConvertTo-SpaceLensVersion $Version
    if (-not $target) { throw "Version '$Version' is not X.Y.Z" }
} elseif ($decision.NextVersion) {
    $target = $decision.NextVersion
}

Write-Host (Format-SpaceLensDryRun -Decision $decision -Head $Head)

if (-not $target) {
    if ($DryRun) {
        Write-Host "prepare_version=(none)"
        Write-Host "dry_run=true"
        exit 0
    }
    throw "no releasable commits since $($latest.Tag); pass -Version only for recovery"
}

$expectedNext = Get-SpaceLensNextPatchVersion $latest
if ($target.Text -ne $expectedNext.Text) {
    throw "V1 prepares the next patch only (wanted $($expectedNext.Text), got $($target.Text))"
}

$notesRel = "docs/release-notes/$($target.Tag).md"
$notesPath = Join-Path $RepoRoot ($notesRel -replace '/', [IO.Path]::DirectorySeparatorChar)
$notes = Format-SpaceLensGeneratedNotes -Version $target -PreviousVersion $latest -Commits $decision.Releasable
$generatedBegin = "<!-- BEGIN GENERATED NOTES -->"
$generatedEnd = "<!-- END GENERATED NOTES -->"
$generatedBlock = ""
if ($notes -match "(?s)$([regex]::Escape($generatedBegin)).*$([regex]::Escape($generatedEnd))") {
    $generatedBlock = $Matches[0]
}

$changelogPath = Join-Path $RepoRoot "CHANGELOG.md"
$changelog = Get-Content -LiteralPath $changelogPath -Raw
$whatChanged = ($notes -split "## What changed", 2)[1]
$whatChanged = ($whatChanged -split "<!-- END GENERATED NOTES -->", 2)[0].Trim()
$section = Format-SpaceLensChangelogSection -Version $target -Date $Date -GeneratedBody $whatChanged

Write-Host "prepare_version=$($target.Text)"
Write-Host "notes=$notesRel"

if ($DryRun) {
    Write-Host "dry_run=true"
    exit 0
}

$changed = Update-SpaceLensMechanicalVersions -RepoRoot $RepoRoot -FromVersion $cmake.Text -ToVersion $target.Text

if (Test-Path -LiteralPath $notesPath) {
    $existing = Get-Content -LiteralPath $notesPath -Raw
    $updatedNotes = Update-SpaceLensGeneratedRegion -Text $existing -BeginMarker $generatedBegin -EndMarker $generatedEnd -Replacement $generatedBlock
    if ($null -eq $updatedNotes) {
        Write-Host "keeping unscoped $notesRel (no generated markers)"
    } else {
        $utf8 = New-Object System.Text.UTF8Encoding $false
        [System.IO.File]::WriteAllText($notesPath, $updatedNotes, $utf8)
    }
} else {
    $notesDir = Split-Path -Parent $notesPath
    if (-not (Test-Path -LiteralPath $notesDir)) {
        New-Item -ItemType Directory -Path $notesDir | Out-Null
    }
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($notesPath, $notes.TrimEnd() + "`n", $utf8)
}

$markerBegin = "<!-- BEGIN GENERATED CHANGELOG $($target.Text) -->"
if ($changelog.Contains($markerBegin)) {
    $updatedLog = Update-SpaceLensGeneratedRegion -Text $changelog -BeginMarker $markerBegin -EndMarker "<!-- END GENERATED CHANGELOG $($target.Text) -->" -Replacement ($markerBegin + "`n" + $whatChanged + "`n<!-- END GENERATED CHANGELOG $($target.Text) -->")
    if ($updatedLog) { $changelog = $updatedLog }
} else {
    $insertAt = $changelog.IndexOf("## [Unreleased]")
    if ($insertAt -lt 0) { throw "CHANGELOG.md missing ## [Unreleased]" }
    $after = $changelog.IndexOf("`n", $insertAt)
    if ($after -lt 0) { throw "CHANGELOG.md Unreleased heading is malformed" }
    $changelog = $changelog.Substring(0, $after + 1) + "`n" + $section.TrimEnd() + "`n" + $changelog.Substring($after + 1)
}
$utf8Log = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($changelogPath, $changelog, $utf8Log)

Write-Host "prepared $($changed.Count) version files plus CHANGELOG and $notesRel"
exit 0
