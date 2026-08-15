[CmdletBinding()]
param(
    [string]$EventName = $env:GITHUB_EVENT_NAME,
    [string]$BaseSha = $env:GITHUB_BASE_REF,
    [string]$BeforeSha = $env:GITHUB_BEFORE_SHA,
    [string]$HeadSha = $env:GITHUB_SHA,
    [string]$EventPath = $env:GITHUB_EVENT_PATH,
    [string]$OutputFile = $env:GITHUB_OUTPUT,
    [string[]]$ManualFileList = @()
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

$statusLines = @()

if ($ManualFileList.Count -gt 0) {
    $statusLines = $ManualFileList
} else {
    try {
        if ($EventName -eq "pull_request") {
            $targetBase = ""
            if ($EventPath -and (Test-Path $EventPath)) {
                try {
                    $json = Get-Content $EventPath -Raw | ConvertFrom-Json
                    $targetBase = $json.pull_request.base.sha
                } catch {}
            }
            if (-not $targetBase -and $BaseSha) {
                $targetBase = "origin/$BaseSha"
            }
            if ($targetBase) {
                $gitOut = git diff --name-status "$targetBase" HEAD 2>$null
                if ($LASTEXITCODE -eq 0 -and $gitOut) {
                    $statusLines = $gitOut -split "`r?\n"
                }
            }
        } elseif ($EventName -eq "push") {
            $before = $BeforeSha
            if ($EventPath -and (Test-Path $EventPath)) {
                try {
                    $json = Get-Content $EventPath -Raw | ConvertFrom-Json
                    $before = $json.before
                } catch {}
            }
            if ($before -and $before -ne "0000000000000000000000000000000000000000") {
                $gitOut = git diff --name-status "$before" HEAD 2>$null
                if ($LASTEXITCODE -eq 0 -and $gitOut) {
                    $statusLines = $gitOut -split "`r?\n"
                }
            } else {
                $gitOut = git diff --name-status HEAD~1 HEAD 2>$null
                if ($LASTEXITCODE -eq 0 -and $gitOut) {
                    $statusLines = $gitOut -split "`r?\n"
                }
            }
        }
    } catch {
        Write-Host "Git diff execution failed; falling back to full CI."
    }
}

$statusLines = $statusLines | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

$allPaths = @()
foreach ($line in $statusLines) {
    $parts = $line -split "`t"
    if ($parts.Count -ge 2) {
        for ($i = 1; $i -lt $parts.Count; $i++) {
            $p = $parts[$i].Trim()
            if ($p) { $allPaths += $p }
        }
    } else {
        $p = $line.Trim()
        if ($p) { $allPaths += $p }
    }
}

Write-Host "Paths inspected ($($allPaths.Count)):"
foreach ($f in $allPaths) {
    Write-Host "  - $f"
}

$cls = Get-SpaceLensCiChangeClass -Paths $allPaths
$docsOnlyStr = if ($cls.DocsOnly) { "true" } else { "false" }
$pinOnlyStr = if ($cls.PinOnly) { "true" } else { "false" }
$runExpensiveStr = if ($cls.RunExpensive) { "true" } else { "false" }

Write-Host "Decision: docs_only=$docsOnlyStr, pin_only=$pinOnlyStr, run_expensive=$runExpensiveStr"

if ($OutputFile) {
    "docs_only=$docsOnlyStr" | Out-File -FilePath $OutputFile -Append
    "pin_only=$pinOnlyStr" | Out-File -FilePath $OutputFile -Append
    "run_expensive=$runExpensiveStr" | Out-File -FilePath $OutputFile -Append
}
