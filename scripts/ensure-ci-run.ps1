# Reuse an existing CI run on this SHA, or dispatch ci.yml once.
# Does not publish, tag, or edit version files.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Sha,
    [Parameter(Mandatory = $true)][string]$Repo,
    [string]$Ref = "main",
    [switch]$DispatchIfMissing,
    [int]$DiscoverSeconds = 90
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

if ($Sha -notmatch '^[0-9a-f]{40}$') {
    throw "Sha '$Sha' is not a 40-char hex commit"
}

function Get-SpaceLensCiWorkflowRuns {
    param([string]$Repository, [string]$Commit)
    $raw = & gh api --paginate "repos/$Repository/actions/workflows/ci.yml/runs?head_sha=$Commit&per_page=20"
    if ($LASTEXITCODE -ne 0) { throw "failed to list ci.yml runs for $Commit" }
    $parsed = $raw | ConvertFrom-Json
    return @($parsed.workflow_runs)
}

function Get-SpaceLensCiDecisionFromApi {
    param([string]$Repository, [string]$Commit)
    $runs = @(Get-SpaceLensCiWorkflowRuns -Repository $Repository -Commit $Commit)
    $slim = @($runs | ForEach-Object {
        [pscustomobject]@{ status = [string]$_.status; conclusion = [string]$_.conclusion; id = [string]$_.id }
    })
    $decision = Get-SpaceLensEnsureCiDecision -Runs $slim
    $best = $null
    if ($decision -eq "reuse") {
        $best = $slim | Where-Object { $_.conclusion -eq "success" } | Select-Object -First 1
        if (-not $best) {
            $best = $slim | Where-Object { $_.status -in @("in_progress", "queued", "waiting", "pending", "requested") } | Select-Object -First 1
        }
    }
    return [pscustomobject]@{
        Decision = $decision
        RunId    = if ($best) { $best.id } else { "" }
        Runs     = $slim
    }
}

$found = Get-SpaceLensCiDecisionFromApi -Repository $Repo -Commit $Sha
if ($found.Decision -eq "reuse") {
    Write-Host "ensure_ci=reuse run_id=$($found.RunId) sha=$Sha"
    if ($env:GITHUB_OUTPUT) {
        "decision=reuse" | Out-File $env:GITHUB_OUTPUT -Append
        "run_id=$($found.RunId)" | Out-File $env:GITHUB_OUTPUT -Append
    }
    exit 0
}

if (-not $DispatchIfMissing) {
    Write-Host "ensure_ci=missing sha=$Sha"
    if ($env:GITHUB_OUTPUT) {
        "decision=missing" | Out-File $env:GITHUB_OUTPUT -Append
        "run_id=" | Out-File $env:GITHUB_OUTPUT -Append
    }
    exit 0
}

$deadline = [datetime]::UtcNow.AddSeconds([Math]::Max(0, $DiscoverSeconds))
while ([datetime]::UtcNow -lt $deadline) {
    Start-Sleep -Seconds 5
    $found = Get-SpaceLensCiDecisionFromApi -Repository $Repo -Commit $Sha
    if ($found.Decision -eq "reuse") {
        Write-Host "ensure_ci=reuse run_id=$($found.RunId) sha=$Sha (appeared while waiting)"
        if ($env:GITHUB_OUTPUT) {
            "decision=reuse" | Out-File $env:GITHUB_OUTPUT -Append
            "run_id=$($found.RunId)" | Out-File $env:GITHUB_OUTPUT -Append
        }
        exit 0
    }
}

$since = [datetime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
Write-Host "Dispatching ci.yml --ref $Ref for $Sha"
& gh workflow run ci.yml --repo $Repo --ref $Ref
if ($LASTEXITCODE -ne 0) { throw "failed to dispatch ci.yml" }

$runId = ""
$pollUntil = [datetime]::UtcNow.AddSeconds(180)
while ([datetime]::UtcNow -lt $pollUntil) {
    Start-Sleep -Seconds 5
    $raw = & gh api "repos/$Repo/actions/workflows/ci.yml/runs?event=workflow_dispatch&per_page=20"
    if ($LASTEXITCODE -ne 0) { throw "failed to list dispatched ci.yml runs" }
    $runs = @((($raw | ConvertFrom-Json).workflow_runs))
    $wrong = $runs | Where-Object { $_.created_at -ge $since -and $_.head_sha -ne $Sha } | Select-Object -First 1
    if ($wrong) {
        throw "CI dispatch attached to $($wrong.head_sha), not bump $Sha. Failing safely."
    }
    $match = $runs | Where-Object { $_.created_at -ge $since -and $_.head_sha -eq $Sha } |
        Sort-Object created_at -Descending |
        Select-Object -First 1
    if ($match) {
        $runId = [string]$match.id
        break
    }
}

if (-not $runId) {
    throw "failed to find ci.yml workflow_dispatch run on $Sha"
}

$head = (& gh api "repos/$Repo/actions/runs/$runId" --jq .head_sha).Trim()
if ($head -ne $Sha) {
    throw "ci.yml run $runId head_sha=$head != $Sha. Failing safely."
}

Write-Host "ensure_ci=dispatch run_id=$runId sha=$Sha"
if ($env:GITHUB_OUTPUT) {
    "decision=dispatch" | Out-File $env:GITHUB_OUTPUT -Append
    "run_id=$runId" | Out-File $env:GITHUB_OUTPUT -Append
}
exit 0
