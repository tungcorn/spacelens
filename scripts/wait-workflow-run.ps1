# Wait for one GitHub Actions run by exact ID.
# gh run watch is used for logs/UI, but a transient API error from the
# watcher is not treated as a child-workflow failure. Fallback polling
# queries the same run_id until it completes or the timeout expires.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RunId,
    [string]$Repo = "",
    [int]$PollTimeoutSeconds = 1800,
    [int]$PollIntervalSeconds = 15,
    [switch]$SkipWatch
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

if (-not $Repo) { $Repo = [string]$env:GITHUB_REPOSITORY }
if (-not $Repo) { throw "Repo is required" }

$result = Wait-SpaceLensChildWorkflow `
    -RunId $RunId `
    -Repo $Repo `
    -PollTimeoutSeconds $PollTimeoutSeconds `
    -PollIntervalSeconds $PollIntervalSeconds `
    -SkipWatch:$SkipWatch

$action = if ($result -is [array]) {
    ($result | Where-Object { $_ -and $_.PSObject.Properties.Name -contains 'Action' } | Select-Object -Last 1).Action
} else {
    $result.Action
}
$reason = if ($result -is [array]) {
    ($result | Where-Object { $_ -and $_.PSObject.Properties.Name -contains 'Reason' } | Select-Object -Last 1).Reason
} else {
    $result.Reason
}

Write-Host "wait_workflow_run action=$action reason=$reason"
if ($action -ne 'continue') {
    exit 1
}
exit 0
