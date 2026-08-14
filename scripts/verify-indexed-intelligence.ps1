# Indexed Intelligence Scaling V1 — exact top-N against a real CLI + isolated index.
# Creates a temp tree (not the source tree, not user data), indexes it under
# SPACELENS_DATA_ROOT, and checks that a late stronger candidate still wins.

[CmdletBinding()]
param(
    [string]$CliPath = "",
    [switch]$KeepFixture
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false
$root = Split-Path -Parent $PSScriptRoot

if (-not $CliPath) {
    foreach ($candidate in @(
            (Join-Path $root "build-debug\cli\spacelens.exe"),
            (Join-Path $root "build-release\cli\spacelens.exe"),
            (Join-Path $root "build-cli-release\cli\spacelens.exe")
        )) {
        if (Test-Path $candidate) {
            $CliPath = $candidate
            break
        }
    }
}
if (-not $CliPath -or -not (Test-Path $CliPath)) {
    Write-Error "CLI not found. Pass -CliPath."
}
$exe = (Resolve-Path $CliPath).Path
Write-Host "Indexed intelligence gate: $exe"

function Write-SizedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int64]$Bytes,
        [int]$DaysAgo = 0
    )
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $buffer = New-Object byte[] ([Math]::Min($Bytes, 4096))
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try {
        $remaining = $Bytes
        while ($remaining -gt 0) {
            $chunk = [int][Math]::Min($remaining, $buffer.Length)
            $fs.Write($buffer, 0, $chunk)
            $remaining -= $chunk
        }
    } finally {
        $fs.Close()
    }
    if ($DaysAgo -ne 0) {
        [System.IO.File]::SetLastWriteTime($Path, (Get-Date).AddDays(-$DaysAgo))
    }
}

function Invoke-CliJson {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [int[]]$AcceptExit = @(0)
    )
    $raw = & $exe @Arguments
    $code = $LASTEXITCODE
    if ($AcceptExit -notcontains $code) {
        Write-Error "spacelens $($Arguments -join ' ') exited $code`n$raw"
    }
    if ([string]::IsNullOrWhiteSpace($raw)) {
        Write-Error "spacelens $($Arguments -join ' ') produced no stdout"
    }
    try {
        $json = $raw | ConvertFrom-Json
    } catch {
        Write-Error "stdout is not JSON: $_`n$raw"
    }
    return [pscustomobject]@{
        Code = $code
        Raw  = [string]$raw
        Json = $json
    }
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        Write-Error $Message
    }
}

function Has-PathNeedle {
    param($Items, [string]$Needle)
    foreach ($item in @($Items)) {
        if ($null -ne $item.path -and ($item.path -like "*$Needle*")) {
            return $true
        }
    }
    return $false
}

$previousDataRoot = $env:SPACELENS_DATA_ROOT
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-idx-intel-" + [guid]::NewGuid().ToString("N"))
$fixture = Join-Path $work "tree"
$dataRoot = Join-Path $work "appdata"
New-Item -ItemType Directory -Force -Path $fixture | Out-Null
New-Item -ItemType Directory -Force -Path $dataRoot | Out-Null
$env:SPACELENS_DATA_ROOT = $dataRoot

try {
    $caps = Invoke-CliJson -Arguments @("capabilities", "--json")
    Assert-True ($caps.Json.filesystem_mutation -eq $false) "capabilities filesystem_mutation is not false"
    Assert-True ($caps.Json.read_only -eq $true) "capabilities read_only is not true"
    Assert-True ($caps.Raw -notmatch "safe_to_delete") "capabilities leaked safe_to_delete"

    # 250 larger Medium BuildArtifact decoys inserted first, then a smaller
    # High-confidence node_modules at the fixture root (not nested under a
    # Temp-path parent that would outrank it). Old size-prefix fetches of
    # 200 would keep only the cmake-build dirs and hide node_modules.
    $decoyCount = 250
    for ($i = 0; $i -lt $decoyCount; $i++) {
        $dir = Join-Path $fixture ("cmake-build-{0}" -f $i)
        Write-SizedFile -Path (Join-Path $dir "CMakeCache.txt") -Bytes 8000 -DaysAgo 5
        Write-SizedFile -Path (Join-Path $dir "out.bin") -Bytes (120KB) -DaysAgo 5
    }
    Write-SizedFile -Path (Join-Path $fixture "node_modules\pkg.js") -Bytes (40KB) -DaysAgo 4
    Write-SizedFile -Path (Join-Path $fixture "tool\.cache\tmp.dat") -Bytes (20KB) -DaysAgo 4
    Write-SizedFile -Path (Join-Path $fixture "keep\only.bin") -Bytes (16KB) -DaysAgo 200

    $indexSw = [System.Diagnostics.Stopwatch]::StartNew()
    $indexed = Invoke-CliJson -Arguments @("index", $fixture, "--json")
    $indexSw.Stop()
    Assert-True ($indexed.Json.ok -eq $true) "index not ok"
    $fileCount = $indexed.Json.index.file_count
    Assert-True ($fileCount -ge ($decoyCount + 3)) "index file_count too small ($fileCount)"
    Write-Host ("index: {0} ms, files={1}" -f $indexSw.ElapsedMilliseconds, $fileCount)

    $oppSw = [System.Diagnostics.Stopwatch]::StartNew()
    $opp = Invoke-CliJson -Arguments @(
        "opportunities", $fixture, "--from-index", "--min-size", "1",
        "--limit", "5", "--json"
    )
    $oppSw.Stop()
    Assert-True ($opp.Json.ok -eq $true) "indexed opportunities not ok"
    Assert-True ($opp.Json.source -eq "persistent_index") "source must be persistent_index"
    Assert-True ($opp.Json.ranking_policy -eq "opportunity_rank_v2") "ranking_policy"
    Assert-True ($opp.Json.filesystem_mutation -eq $false) "opportunities mutation"
    Assert-True ($opp.Json.planning_only -eq $true) "planning_only"
    Assert-True ($opp.Raw -notmatch "safe_to_delete") "opportunities leaked safe_to_delete"
    Assert-True (Has-PathNeedle $opp.Json.opportunities "node_modules") `
        "hidden node_modules missing from top-5 after $decoyCount decoys"
    Assert-True ($opp.Json.opportunities[0].classification -eq "DependencyDirectory") `
        "first row must be the High-confidence node_modules, not a larger Medium decoy"
    Assert-True ($opp.Json.opportunities[0].path -like "*node_modules*") `
        "first path is not node_modules"
    Assert-True ($opp.Json.opportunities[0].path -notlike "*cmake-build*") `
        "size-prefix ranking leaked a cmake-build decoy into rank 1"
    Assert-True ($opp.Json.summary.returned_count -le 5) "limit 5 exceeded"
    Assert-True ($opp.Json.summary.truncated -eq $true) "expected truncated after 250+ matches"
    $jsonBytes = [Text.Encoding]::UTF8.GetByteCount($opp.Raw)
    Assert-True ($jsonBytes -lt 200KB) "opportunities JSON too large ($jsonBytes)"
    Write-Host ("unfiltered top-5: {0} ms, json={1} bytes, first={2}" -f `
            $oppSw.ElapsedMilliseconds, $jsonBytes, $opp.Json.opportunities[0].path)

    $cls = Invoke-CliJson -Arguments @(
        "opportunities", $fixture, "--from-index", "--min-size", "1",
        "--classification", "TemporaryData", "--limit", "20", "--json"
    )
    Assert-True ($cls.Json.ok -eq $true) "classification filter not ok"
    Assert-True (Has-PathNeedle $cls.Json.opportunities ".cache") `
        "TemporaryData .cache hidden by BuildArtifact prefix"
    foreach ($item in @($cls.Json.opportunities)) {
        Assert-True ($item.classification -eq "TemporaryData") "classification leak"
    }

    $none = Invoke-CliJson -Arguments @(
        "opportunities", $fixture, "--from-index",
        "--classification", "BuildArtfact", "--json"
    )
    Assert-True ($none.Json.ok -eq $true) "unknown class should succeed empty"
    Assert-True (@($none.Json.opportunities).Count -eq 0) "unknown class must match nothing"

    $underPath = Join-Path $fixture "keep"
    $under = Invoke-CliJson -Arguments @(
        "opportunities", $fixture, "--from-index", "--min-size", "1",
        "--under", $underPath, "--limit", "10", "--json"
    )
    Assert-True ($under.Json.ok -eq $true) "--under opportunities not ok"
    Assert-True (Has-PathNeedle $under.Json.opportunities "only.bin") "--under missed keep\only.bin"
    foreach ($item in @($under.Json.opportunities)) {
        Assert-True ($item.path -like "*\keep*") "--under leaked path $($item.path)"
        Assert-True ($item.path -notlike "*cmake-build*") "--under leaked decoy"
    }

    $q = Invoke-CliJson -Arguments @(
        "query", $fixture, "--files", "--under", $underPath, "--limit", "5", "--json"
    )
    Assert-True ($q.Json.ok -eq $true) "query --under not ok"
    Assert-True ($q.Json.returned_items -ge 1) "query --under empty"

    $unique = [uint64]$opp.Json.summary.unique_review_bytes
    $logical = [uint64]$opp.Json.summary.logical_bytes
    Assert-True ($unique -le $logical) "unique_review_bytes exceeds root logical bytes"
    Assert-True ($opp.Json.summary.unique_review_estimated -eq $false) `
        "unique_review_estimated must be false for this small matching set"
    Write-Host ("unique_review_bytes={0} estimated={1}" -f `
            $unique, $opp.Json.summary.unique_review_estimated)
    Write-Host "INDEXED_INTELLIGENCE_SCALING_V1 + exact aggregates verify script passed"
} finally {
    if ($null -eq $previousDataRoot -or $previousDataRoot -eq "") {
        Remove-Item Env:SPACELENS_DATA_ROOT -ErrorAction SilentlyContinue
    } else {
        $env:SPACELENS_DATA_ROOT = $previousDataRoot
    }
    if (-not $KeepFixture) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host "Kept fixture: $work"
    }
}
