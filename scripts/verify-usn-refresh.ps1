#Requires -Version 5.1
<#
.SYNOPSIS
  Elevated verification of SpaceLens USN incremental refresh vs full rebuild.

.DESCRIPTION
  Creates a temporary fixture tree, full-indexes it, mutates it, runs
  `index refresh`, captures query snapshots, full-rebuilds, and compares.
  Also times full rebuild vs incremental batches when USN is available.

  Safe: only touches a generated temp tree + AppData index metadata.
  Does NOT create/configure the USN journal.
  Does NOT mutate user project files.

.EXAMPLE
  # From an elevated PowerShell:
  . .\scripts\dev-env.ps1
  .\scripts\verify-usn-refresh.ps1 -CliPath .\build-release\cli\spacelens.exe

  # Or self-elevate:
  .\scripts\verify-usn-refresh.ps1 -SelfElevate -CliPath .\build-release\cli\spacelens.exe
#>
[CmdletBinding()]
param(
    [string]$CliPath = "",
    [int]$LargeFileCount = 20000,
    [switch]$SelfElevate,
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"

function Test-IsElevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Resolve-Cli {
    param([string]$PathHint)
    if ($PathHint -and (Test-Path $PathHint)) {
        return (Resolve-Path $PathHint).Path
    }
    $candidates = @(
        ".\build-release\cli\spacelens.exe",
        ".\build\cli\spacelens.exe",
        "build-release\cli\spacelens.exe",
        "build\cli\spacelens.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    throw "spacelens.exe not found. Build Release CLI first."
}

function Invoke-CliJson {
    param([string]$Cli, [string[]]$CliArgs)
    # Capture as string array (never pipe a single string — PS enumerates chars).
    $output = & $Cli @CliArgs 2>&1
    $lines = @()
    foreach ($item in @($output)) {
        $s = "$item".Trim()
        if ($s) { $lines += $s }
    }
    $line = ($lines | Where-Object { $_.StartsWith('{') } | Select-Object -Last 1)
    if (-not $line) {
        throw "CLI produced no JSON object for: $($CliArgs -join ' ') :: $($lines -join ' | ')"
    }
    return ($line | ConvertFrom-Json)
}

function New-FixtureTree {
    param([string]$Root, [int]$FileCount)
    New-Item -ItemType Directory -Force -Path $Root | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "sub\nested") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "build\obj") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "models") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "outside") | Out-Null
    Set-Content -Path (Join-Path $Root "readme.txt") -Value "hello world" -NoNewline
    Set-Content -Path (Join-Path $Root "sub\data.bin") -Value ("x" * 4096) -NoNewline
    Set-Content -Path (Join-Path $Root "sub\nested\leaf.txt") -Value "leaf" -NoNewline
    Set-Content -Path (Join-Path $Root "build\obj\a.obj") -Value ("o" * 512) -NoNewline
    Set-Content -Path (Join-Path $Root "models\sample.gguf") -Value ("g" * 2048) -NoNewline
    Set-Content -Path (Join-Path $Root "outside\ext.txt") -Value "external" -NoNewline

    # Bulk files for benchmark scale
    $bulk = Join-Path $Root "bulk"
    New-Item -ItemType Directory -Force -Path $bulk | Out-Null
    for ($i = 0; $i -lt $FileCount; $i++) {
        $d = Join-Path $bulk ("d{0:D3}" -f ($i % 100))
        if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
        $p = Join-Path $d ("f{0:D5}.txt" -f $i)
        Set-Content -Path $p -Value ("x" * (($i % 64) + 1)) -NoNewline
    }
}

function Get-QuerySnapshot {
    param([string]$Cli, [string]$Root)
    $files = Invoke-CliJson $Cli @("query", $Root, "--files", "--limit", "100000", "--json")
    $dirs  = Invoke-CliJson $Cli @("query", $Root, "--dirs",  "--limit", "100000", "--json")
    $status = Invoke-CliJson $Cli @("index", "status", $Root, "--json")
    $rows = @()
    if ($files.results) {
        foreach ($r in $files.results) {
            $rows += [pscustomobject]@{
                path = [string]$r.path
                kind = "file"  # CLI uses "file"
                size_bytes = [int64]$r.size_bytes
                classification = [string]$r.classification
                confidence = [string]$r.confidence
                reclaimability = [string]$r.reclaimability
                candidate_strength = [string]$r.candidate_strength
            }
        }
    }
    if ($dirs.results) {
        foreach ($r in $dirs.results) {
            $rows += [pscustomobject]@{
                path = [string]$r.path
                kind = "directory"
                size_bytes = [int64]$r.size_bytes
                classification = [string]$r.classification
                confidence = [string]$r.confidence
                reclaimability = [string]$r.reclaimability
                candidate_strength = [string]$r.candidate_strength
            }
        }
    }
    $rows = $rows | Sort-Object path, kind
    return [pscustomobject]@{
        file_count = [int64]$status.index.file_count
        directory_count = [int64]$status.index.directory_count
        logical_bytes = [int64]$status.index.logical_bytes
        rows = $rows
    }
}

function Compare-Snapshots {
    param($Inc, $Full, [string]$Label)
    $errors = New-Object System.Collections.Generic.List[string]
    if ($Inc.file_count -ne $Full.file_count) {
        $errors.Add("$Label file_count inc=$($Inc.file_count) full=$($Full.file_count)")
    }
    if ($Inc.directory_count -ne $Full.directory_count) {
        $errors.Add("$Label directory_count inc=$($Inc.directory_count) full=$($Full.directory_count)")
    }
    if ($Inc.logical_bytes -ne $Full.logical_bytes) {
        $errors.Add("$Label logical_bytes inc=$($Inc.logical_bytes) full=$($Full.logical_bytes)")
    }
    if ($Inc.rows.Count -ne $Full.rows.Count) {
        $errors.Add("$Label row_count inc=$($Inc.rows.Count) full=$($Full.rows.Count)")
    } else {
        for ($i = 0; $i -lt $Inc.rows.Count; $i++) {
            $a = $Inc.rows[$i]; $b = $Full.rows[$i]
            if ($a.path -ne $b.path -or $a.kind -ne $b.kind -or
                $a.size_bytes -ne $b.size_bytes -or
                $a.classification -ne $b.classification -or
                $a.confidence -ne $b.confidence -or
                $a.reclaimability -ne $b.reclaimability -or
                $a.candidate_strength -ne $b.candidate_strength) {
                $errors.Add("$Label row[$i] mismatch path_inc=$($a.path) path_full=$($b.path) size_inc=$($a.size_bytes) size_full=$($b.size_bytes)")
                if ($errors.Count -gt 20) { break }
            }
        }
    }
    return $errors
}

# --- self-elevate ---
if ($SelfElevate -and -not (Test-IsElevated)) {
    $script = $MyInvocation.MyCommand.Path
    $cliArg = if ($CliPath) { "-CliPath `"$CliPath`"" } else { "" }
    $reportArg = if ($ReportPath) { "-ReportPath `"$ReportPath`"" } else { "" }
    $args = "-NoProfile -ExecutionPolicy Bypass -File `"$script`" $cliArg -LargeFileCount $LargeFileCount $reportArg"
    Write-Host "Requesting elevation (UAC)..."
    $p = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $args -Wait -PassThru
    exit $p.ExitCode
}

$cli = Resolve-Cli -PathHint $CliPath
$elevated = Test-IsElevated
if (-not $ReportPath) {
    $ReportPath = Join-Path (Split-Path $cli -Parent) "..\..\usn-verify-last-run.json"
    $ReportPath = [IO.Path]::GetFullPath($ReportPath)
}

$report = [ordered]@{
    schema_version = 1
    started_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    elevated = $elevated
    user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    cli = $cli
    filesystem = $null
    journal_query = $null
    volume_open = $null
    usn_access = $null
    scenarios = @()
    benchmarks = @()
    outcome = "unknown"
    notes = @()
}

# Journal / FS probe
try {
    $drive = (Get-Item $env:TEMP).PSDrive.Name
    $vol = Get-Volume -DriveLetter $drive -ErrorAction SilentlyContinue
    if ($vol) {
        $report.filesystem = [ordered]@{
            drive = $drive
            type = "$($vol.FileSystemType)"
            health = "$($vol.HealthStatus)"
        }
    }
    $jq = fsutil usn queryjournal "${drive}:" 2>&1 | Out-String
    $report.journal_query = $jq.Trim()
} catch {
    $report.notes += "journal_query_failed: $($_.Exception.Message)"
}

try {
    $drive = (Get-Item $env:TEMP).PSDrive.Name
    $fs = [System.IO.File]::Open("\\.\$drive`:", 'Open', 'Read', 'ReadWrite')
    $fs.Close()
    $report.volume_open = "ok"
} catch {
    $report.volume_open = "fail: $($_.Exception.Message)"
}

$fixture = Join-Path $env:TEMP ("spacelens_usn_verify_" + [guid]::NewGuid().ToString("N"))
$subdirRoot = Join-Path $fixture "sub"
New-FixtureTree -Root $fixture -FileCount 50  # small suite first

Write-Host "Fixture: $fixture"
Write-Host "Elevated: $elevated"
Write-Host "CLI: $cli"

# Full index
$idx = Invoke-CliJson $cli @("index", $fixture, "--json")
$status = Invoke-CliJson $cli @("index", "status", $fixture, "--json")
$report.usn_access = $status.incremental_refresh
$incState = [string]$status.incremental_refresh.state
$incReason = [string]$status.incremental_refresh.reason

if ($incState -ne "supported" -and [string]$status.incremental_refresh.checkpoint.status -ne "ready") {
    # Try refresh to confirm
    $ref = Invoke-CliJson $cli @("index", "refresh", $fixture, "--json")
    $report.scenarios += [ordered]@{
        name = "probe_after_full_index"
        index_ok = [bool]$idx.ok
        status = $status.incremental_refresh
        refresh_outcome = $ref.outcome
        refresh_reason = $ref.reason
    }
    $report.outcome = "environment_blocked"
    $report.notes += "USN checkpoint not ready after full index (state=$incState reason=$incReason). Volume open=$($report.volume_open). Elevated=$elevated."
    $report.finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    $report | ConvertTo-Json -Depth 8 | Set-Content -Path $ReportPath -Encoding UTF8
    Write-Host "ENVIRONMENT_BLOCKED - report: $ReportPath"
    Write-Host ($report.notes -join "`n")
    exit 2
}

function Invoke-MutationParity {
    param([string]$Root, [string]$Label, [scriptblock]$Mutate)
    & $Mutate
    Start-Sleep -Milliseconds 100
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $ref = Invoke-CliJson $cli @("index", "refresh", $Root, "--json")
    $sw.Stop()
    if ($ref.outcome -eq "full_rebuild_required" -or $ref.ok -eq $false -and $ref.outcome -ne "already_current" -and $ref.outcome -ne "refreshed") {
        return [ordered]@{
            name = $Label
            ok = $false
            refresh = $ref
            refresh_ms = $sw.ElapsedMilliseconds
            error = "refresh failed: $($ref.outcome) $($ref.reason)"
        }
    }
    $incSnap = Get-QuerySnapshot -Cli $cli -Root $Root
    $swFull = [Diagnostics.Stopwatch]::StartNew()
    $full = Invoke-CliJson $cli @("index", $Root, "--json")
    $swFull.Stop()
    $fullSnap = Get-QuerySnapshot -Cli $cli -Root $Root
    $errs = Compare-Snapshots -Inc $incSnap -Full $fullSnap -Label $Label
    return [ordered]@{
        name = $Label
        ok = ($errs.Count -eq 0)
        refresh = $ref
        refresh_ms = $sw.ElapsedMilliseconds
        full_rebuild_ms = $swFull.ElapsedMilliseconds
        parity_errors = @($errs)
        inc_counts = @{ files = $incSnap.file_count; dirs = $incSnap.directory_count; bytes = $incSnap.logical_bytes; rows = $incSnap.rows.Count }
        full_counts = @{ files = $fullSnap.file_count; dirs = $fullSnap.directory_count; bytes = $fullSnap.logical_bytes; rows = $fullSnap.rows.Count }
    }
}

# Re-index clean after any prior full rebuild in probe
$null = Invoke-CliJson $cli @("index", $fixture, "--json")

$scenarios = @()
$scenarios += Invoke-MutationParity -Root $fixture -Label "create_modify_delete_rename" -Mutate {
    Set-Content -Path (Join-Path $fixture "new_file.dat") -Value ("n" * 8192) -NoNewline
    Set-Content -Path (Join-Path $fixture "readme.txt") -Value "hello world!!!" -NoNewline
    Remove-Item -Force (Join-Path $fixture "sub\data.bin")
    Set-Content -Path (Join-Path $fixture "sub\c.txt") -Value "created" -NoNewline
    Rename-Item (Join-Path $fixture "models\sample.gguf") "sample_renamed.gguf"
    New-Item -ItemType Directory -Force -Path (Join-Path $fixture "sub\moved_here") | Out-Null
    Move-Item (Join-Path $fixture "sub\nested\leaf.txt") (Join-Path $fixture "sub\moved_here\leaf.txt")
    Rename-Item (Join-Path $fixture "build\obj") "objects"
}

# Boundary: index subdir only
$null = Invoke-CliJson $cli @("index", $subdirRoot, "--json")
$scenarios += Invoke-MutationParity -Root $subdirRoot -Label "subdir_boundary_in_out" -Mutate {
    Move-Item (Join-Path $fixture "outside\ext.txt") (Join-Path $subdirRoot "from_outside.txt") -Force
    if (Test-Path (Join-Path $subdirRoot "c.txt")) {
        Move-Item (Join-Path $subdirRoot "c.txt") (Join-Path $fixture "outside\c_out.txt") -Force
    }
    Set-Content -Path (Join-Path $subdirRoot "only_inside.txt") -Value "inside" -NoNewline
}

$report.scenarios = $scenarios
$allOk = ($scenarios | Where-Object { -not $_.ok }).Count -eq 0

# Large-tree benchmarks (only if parity ok)
if ($allOk) {
    $large = Join-Path $env:TEMP ("spacelens_usn_bench_" + [guid]::NewGuid().ToString("N"))
    Write-Host "Building large fixture (${LargeFileCount} files): $large"
    New-FixtureTree -Root $large -FileCount $LargeFileCount
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $li = Invoke-CliJson $cli @("index", $large, "--json")
    $sw.Stop()
    $report.benchmarks += [ordered]@{
        name = "full_rebuild"
        files = $LargeFileCount
        process_ms = $sw.ElapsedMilliseconds
        engine_elapsed_ms = $li.elapsed_ms
        ok = [bool]$li.ok
    }

    # 1 change
    Set-Content -Path (Join-Path $large "bench_one.txt") -Value "one" -NoNewline
    Start-Sleep -Milliseconds 80
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $r1 = Invoke-CliJson $cli @("index", "refresh", $large, "--json")
    $sw.Stop()
    $report.benchmarks += [ordered]@{
        name = "incremental_1_change"
        process_ms = $sw.ElapsedMilliseconds
        outcome = $r1.outcome
        journal_records_seen = $r1.journal_records_seen
        added = $r1.added
        removed = $r1.removed
        modified = $r1.modified
        rows_changed = $r1.rows_changed
        engine_elapsed_ms = $r1.elapsed_ms
    }

    # 100 changes
    for ($i = 0; $i -lt 100; $i++) {
        Set-Content -Path (Join-Path $large "bulk\d000\b$i.txt") -Value ("b" * ($i + 1)) -NoNewline
    }
    Start-Sleep -Milliseconds 80
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $r100 = Invoke-CliJson $cli @("index", "refresh", $large, "--json")
    $sw.Stop()
    $report.benchmarks += [ordered]@{
        name = "incremental_100_changes"
        process_ms = $sw.ElapsedMilliseconds
        outcome = $r100.outcome
        journal_records_seen = $r100.journal_records_seen
        added = $r100.added
        removed = $r100.removed
        modified = $r100.modified
        rows_changed = $r100.rows_changed
        engine_elapsed_ms = $r100.elapsed_ms
    }

    # large batch ~1000
    for ($i = 0; $i -lt 1000; $i++) {
        $p = Join-Path $large ("bulk\d001\batch{0:D4}.txt" -f $i)
        Set-Content -Path $p -Value ("z" * 8) -NoNewline
    }
    Start-Sleep -Milliseconds 80
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $r1000 = Invoke-CliJson $cli @("index", "refresh", $large, "--json")
    $sw.Stop()
    $report.benchmarks += [ordered]@{
        name = "incremental_1000_changes"
        process_ms = $sw.ElapsedMilliseconds
        outcome = $r1000.outcome
        journal_records_seen = $r1000.journal_records_seen
        added = $r1000.added
        removed = $r1000.removed
        modified = $r1000.modified
        rows_changed = $r1000.rows_changed
        engine_elapsed_ms = $r1000.elapsed_ms
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $q = Invoke-CliJson $cli @("query", $large, "--files", "--limit", "20", "--json")
    $sw.Stop()
    $report.benchmarks += [ordered]@{
        name = "query_after_refresh"
        process_ms = $sw.ElapsedMilliseconds
        ok = [bool]$q.ok
        returned = $q.returned_items
    }

    # cleanup large fixture files (best effort)
    try { Remove-Item -Recurse -Force $large -ErrorAction SilentlyContinue } catch {}
}

if ($allOk) {
    $report.outcome = "pass"
} else {
    $report.outcome = "fail"
    $report.notes += "One or more parity scenarios failed."
}

$report.finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
$report | ConvertTo-Json -Depth 10 | Set-Content -Path $ReportPath -Encoding UTF8
Write-Host "Outcome: $($report.outcome)"
Write-Host "Report: $ReportPath"
foreach ($s in $scenarios) {
    Write-Host ("  scenario {0}: ok={1} refresh_ms={2} full_ms={3}" -f $s.name, $s.ok, $s.refresh_ms, $s.full_rebuild_ms)
    if ($s.parity_errors) { $s.parity_errors | ForEach-Object { Write-Host "    $_" } }
}

# cleanup small fixture
try { Remove-Item -Recurse -Force $fixture -ErrorAction SilentlyContinue } catch {}

if ($report.outcome -eq "pass") { exit 0 }
if ($report.outcome -eq "environment_blocked") { exit 2 }
exit 1
