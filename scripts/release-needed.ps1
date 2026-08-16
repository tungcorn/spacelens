# Classify commits since the latest stable tag and print a dry-run plan.
# Does not create tags, releases, npm packages, or commits.

[CmdletBinding()]
param(
    [switch]$DryRun,
    [string]$SinceTag = "",
    [string]$Head = "HEAD",
    [string]$RepoRoot = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

if (-not $RepoRoot) { $RepoRoot = Get-SpaceLensRepoRoot }

$tags = Get-SpaceLensGitTags -RepoRoot $RepoRoot
$latest = $null
if ($SinceTag) {
    $latest = ConvertTo-SpaceLensVersion $SinceTag
    if (-not $latest) { throw "SinceTag '$SinceTag' is not a stable vX.Y.Z" }
} else {
    $latest = Get-SpaceLensLatestStableVersion -Tags $tags
}
if (-not $latest) { throw "no stable vX.Y.Z tag found" }

$commits = @(Get-SpaceLensCommitsSinceTag -Tag $latest.Tag -Head $Head -RepoRoot $RepoRoot)
$decision = Get-SpaceLensReleaseDecision -Commits $commits -LatestVersion $latest
$report = Format-SpaceLensDryRun -Decision $decision -Head $Head
Write-Host $report

if ($env:GITHUB_OUTPUT) {
    "releasable=$($decision.Needed.ToString().ToLowerInvariant())" | Out-File $env:GITHUB_OUTPUT -Append
    $nextText = if ($decision.NextVersion) { $decision.NextVersion.Text } else { "" }
    $nextTag = if ($decision.NextVersion) { $decision.NextVersion.Tag } else { "" }
    "next_version=$nextText" | Out-File $env:GITHUB_OUTPUT -Append
    "next_tag=$nextTag" | Out-File $env:GITHUB_OUTPUT -Append
    "latest_version=$($latest.Text)" | Out-File $env:GITHUB_OUTPUT -Append
}

if ($decision.Needed) { exit 0 }
# Not an error: docs-only ranges are a successful "no release" result.
exit 0
