# Decide whether this main push (or dispatch) should run the publication pipeline.
# Does not tag, publish, or write version files.

[CmdletBinding()]
param(
    [string]$EventName = "",
    [string]$RequestedVersion = "",
    [string]$PublishInput = "",
    [string]$HeadSha = "",
    [string]$RepoRoot = "",
    [string]$WorkflowRunConclusion = "",
    [string]$WorkflowRunBranch = "",
    [string]$WorkflowRunEvent = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

if (-not $RepoRoot) { $RepoRoot = Get-SpaceLensRepoRoot }
if (-not $EventName) { $EventName = [string]$env:GITHUB_EVENT_NAME }
if (-not $RequestedVersion) { $RequestedVersion = [string]$env:REQUESTED_VERSION }
if (-not $PublishInput) { $PublishInput = [string]$env:PUBLISH_INPUT }
if (-not $HeadSha) { $HeadSha = [string]$env:HEAD_SHA }
if (-not $HeadSha) { $HeadSha = [string]$env:GITHUB_SHA }
if (-not $WorkflowRunConclusion) { $WorkflowRunConclusion = [string]$env:WORKFLOW_RUN_CONCLUSION }
if (-not $WorkflowRunBranch) { $WorkflowRunBranch = [string]$env:WORKFLOW_RUN_BRANCH }
if (-not $WorkflowRunEvent) { $WorkflowRunEvent = [string]$env:WORKFLOW_RUN_EVENT }

$cmake = Get-SpaceLensCMakeVersion -RepoRoot $RepoRoot
$npm = Get-SpaceLensNpmVersion -RepoRoot $RepoRoot
if ($cmake.Text -ne $npm.Text) {
    throw "CMake $($cmake.Text) != package.json $($npm.Text)"
}

$latest = Get-SpaceLensLatestStableVersion -Tags (Get-SpaceLensGitTags -RepoRoot $RepoRoot)
if (-not $latest) { throw "no stable vX.Y.Z tag found" }
$next = Get-SpaceLensNextPatchVersion $latest
if (-not $HeadSha) {
    $HeadSha = (& git -C $RepoRoot rev-parse HEAD).Trim()
}

$version = $cmake
$publish = $false
$runPipeline = $false
$reason = ""
$prepareNeeded = $false

$commits = Get-SpaceLensCommitsSinceTag -Tag $latest.Tag -Head $HeadSha -RepoRoot $RepoRoot
$releaseDecision = Get-SpaceLensReleaseDecision -Commits $commits -LatestVersion $latest

if ($EventName -eq "workflow_dispatch") {
    if (-not $RequestedVersion) {
        throw "workflow_dispatch requires a version input"
    }
    $requested = ConvertTo-SpaceLensVersion $RequestedVersion
    if (-not $requested) { throw "version '$RequestedVersion' is not X.Y.Z" }
    if ($requested.Text -ne $cmake.Text) {
        throw "requested $($requested.Text) does not match CMake $($cmake.Text)"
    }
    $version = $requested
    $publish = ($PublishInput -eq "true")
    $runPipeline = $true
    $reason = if ($publish) {
        "workflow_dispatch publish recovery for $($version.Text)"
    } else {
        "workflow_dispatch dry-run for $($version.Text)"
    }
} else {
    $version = $cmake
    $parentCmake = $null
    $parent = & git -C $RepoRoot rev-parse --verify "${HeadSha}^" 2>$null
    if ($LASTEXITCODE -eq 0 -and "$parent".Trim()) {
        $parentText = (& git -C $RepoRoot show "$($parent.Trim()):CMakeLists.txt" | Out-String)
        if ($LASTEXITCODE -eq 0 -and $parentText) {
            try { $parentCmake = Get-SpaceLensCMakeVersionFromText $parentText } catch { $parentCmake = $null }
        }
    }
    $tagPeel = Get-SpaceLensTagPeelSha -Tag $cmake.Tag -RepoRoot $RepoRoot
    $auto = Test-SpaceLensAutoPublishCommit `
        -CMakeVersion $cmake `
        -LatestStableVersion $latest `
        -ParentCMakeVersion $parentCmake `
        -HeadSha $HeadSha `
        -TagPeelSha $tagPeel
    if ($auto) {
        $publish = $true
        $runPipeline = $true
        $reason = "CMake $($cmake.Text) is the next patch after $($latest.Text) and this commit is the version bump or already tagged"
    } elseif ($cmake.Text -eq $next.Text) {
        $reason = "CMake $($cmake.Text) is the next patch $($next.Text), but this commit did not bump it (tag does not point here)"
    } else {
        $reason = "CMake $($cmake.Text) is not the next patch $($next.Text) of $($latest.Text)"
    }
}

$prepareNeeded = Get-SpaceLensPrepareNeed `
    -RunPipeline $runPipeline `
    -EventName $EventName `
    -ReleaseNeeded ([bool]$releaseDecision.Needed) `
    -WorkflowRunConclusion $WorkflowRunConclusion `
    -WorkflowRunBranch $WorkflowRunBranch `
    -WorkflowRunEvent $WorkflowRunEvent `
    -HeadSha $HeadSha

if ($EventName -eq "workflow_run") {
    if ([string]::IsNullOrWhiteSpace($HeadSha)) {
        $reason = "source CI head SHA was empty"
    } elseif ($WorkflowRunConclusion -ne "success") {
        $reason = "source CI conclusion was '$WorkflowRunConclusion' (not success)"
    } elseif ($WorkflowRunBranch -ne "main") {
        $reason = "source CI branch was '$WorkflowRunBranch' (not main)"
    } elseif ($WorkflowRunEvent -ne "push") {
        $reason = "source CI event was '$WorkflowRunEvent' (not push)"
    }
}

Write-Host "decide_event=$EventName"
Write-Host "decide_version=$($version.Text)"
Write-Host "decide_latest=$($latest.Text)"
Write-Host "decide_next_patch=$($next.Text)"
Write-Host "decide_publish=$($publish.ToString().ToLowerInvariant())"
Write-Host "decide_run_pipeline=$($runPipeline.ToString().ToLowerInvariant())"
Write-Host "decide_prepare_needed=$($prepareNeeded.ToString().ToLowerInvariant())"
Write-Host "decide_release_needed=$($releaseDecision.Needed.ToString().ToLowerInvariant())"
Write-Host "decide_reason=$reason"

if ($env:GITHUB_OUTPUT) {
    "version=$($version.Text)" | Out-File $env:GITHUB_OUTPUT -Append
    "tag=$($version.Tag)" | Out-File $env:GITHUB_OUTPUT -Append
    "publish=$($publish.ToString().ToLowerInvariant())" | Out-File $env:GITHUB_OUTPUT -Append
    "run_pipeline=$($runPipeline.ToString().ToLowerInvariant())" | Out-File $env:GITHUB_OUTPUT -Append
    "prepare_needed=$($prepareNeeded.ToString().ToLowerInvariant())" | Out-File $env:GITHUB_OUTPUT -Append
    "reason=$reason" | Out-File $env:GITHUB_OUTPUT -Append
}

exit 0
