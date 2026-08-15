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

function Test-IsDocFile([string]$path) {
    $p = $path.Replace("\", "/").Trim().ToLowerInvariant()
    if ([string]::IsNullOrWhiteSpace($p)) { return $false }

    if ($p -eq "readme.md" -or $p -eq "changelog.md" -or $p -eq "contributing.md" -or $p -eq "license") {
        return $true
    }
    if ($p.StartsWith("docs/")) {
        return $true
    }
    if ($p.EndsWith(".md")) {
        if ($p.StartsWith("src/") -or $p.StartsWith("tests/") -or $p.StartsWith("scripts/") -or $p.StartsWith("packaging/") -or $p.StartsWith(".github/") -or $p.StartsWith("cmake/") -or $p.StartsWith("third_party/")) {
            return $false
        }
        return $true
    }
    return $false
}

$changedFiles = @()

if ($ManualFileList.Count -gt 0) {
    $changedFiles = $ManualFileList
} else {
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
            $gitOut = git diff --name-only "$targetBase" HEAD 2>$null
            if ($LASTEXITCODE -eq 0 -and $gitOut) {
                $changedFiles = $gitOut -split "`r?\n"
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
            $gitOut = git diff --name-only "$before" HEAD 2>$null
            if ($LASTEXITCODE -eq 0 -and $gitOut) {
                $changedFiles = $gitOut -split "`r?\n"
            }
        } else {
            $gitOut = git diff --name-only HEAD~1 HEAD 2>$null
            if ($LASTEXITCODE -eq 0 -and $gitOut) {
                $changedFiles = $gitOut -split "`r?\n"
            }
        }
    }
}

$changedFiles = $changedFiles | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

Write-Host "Changed files detected ($($changedFiles.Count)):"
foreach ($f in $changedFiles) {
    Write-Host "  - $f"
}

$runExpensive = $true
$docsOnly = $false

if ($changedFiles.Count -gt 0) {
    $nonDocFound = $false
    foreach ($f in $changedFiles) {
        if (-not (Test-IsDocFile $f)) {
            $nonDocFound = $true
            Write-Host "Non-doc file detected: $f"
            break
        }
    }
    if (-not $nonDocFound) {
        $docsOnly = $true
        $runExpensive = $false
    }
} else {
    Write-Host "No changed file list available or 0 files changed. Defaulting to full CI."
}

Write-Host "Decision: docs_only=$docsOnly, run_expensive=$runExpensive"

if ($OutputFile) {
    "docs_only=$docsOnly" | Out-File -FilePath $OutputFile -Append
    "run_expensive=$runExpensive" | Out-File -FilePath $OutputFile -Append
}
