# Generated-fixture stress / performance harness for Release SpaceLens CLI.
# Never points at user data or the project source tree.

[CmdletBinding()]
param(
    [int]$Entries = 100000,
    [string]$CliPath = "",
    [string]$WorkRoot = "",
    [int]$QueryRepeats = 11,
    [switch]$KeepFixture,
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $CliPath) {
    $CliPath = Join-Path $root "build-release\cli\spacelens.exe"
}
if (-not (Test-Path $CliPath)) {
    Write-Error "Release CLI not found: $CliPath"
}
$CliPath = (Resolve-Path $CliPath).Path

if (-not $WorkRoot) {
    $WorkRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-stress-" + [guid]::NewGuid().ToString("N"))
}
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
$fixture = Join-Path $WorkRoot "fixture"
$reportDir = Join-Path $WorkRoot "out"
New-Item -ItemType Directory -Force -Path $fixture, $reportDir | Out-Null

if (-not $ReportPath) {
    $ReportPath = Join-Path $reportDir "stress-v01.json"
}

function New-StressFixture([string]$Root, [int]$TargetEntries) {
    Write-Host "Generating fixture under $Root (target >= $TargetEntries entries)"
    $files = 0
    $dirs = 1
    $bytes = [int64]0

    $wide = Join-Path $Root "wide"
    $nested = Join-Path $Root "nested"
    $unicode = Join-Path $Root "unicode-名称"
    $deep = Join-Path $Root "deep"
    [void][System.IO.Directory]::CreateDirectory($wide)
    [void][System.IO.Directory]::CreateDirectory($nested)
    [void][System.IO.Directory]::CreateDirectory($unicode)
    [void][System.IO.Directory]::CreateDirectory($deep)
    $dirs += 4

    $wideDirCount = 20
    $wideDirs = @()
    for ($d = 0; $d -lt $wideDirCount; $d++) {
        $path = Join-Path $wide ("bucket-{0:D2}" -f $d)
        [void][System.IO.Directory]::CreateDirectory($path)
        $wideDirs += $path
        $dirs++
    }

    $payloadEmpty = [byte[]]@()
    $payloadSmall = [byte[]](1..16)
    $payloadMid = New-Object byte[] 4096

    $remaining = [Math]::Max(0, $TargetEntries - $dirs - 80)
    $perBucket = [Math]::Max(1, [int][Math]::Ceiling($remaining / [double]$wideDirCount))
    for ($d = 0; $d -lt $wideDirCount; $d++) {
        $dir = $wideDirs[$d]
        for ($i = 0; $i -lt $perBucket; $i++) {
            $name = "f-{0:D6}.dat" -f $i
            $path = Join-Path $dir $name
            switch ($i % 7) {
                0 { [System.IO.File]::WriteAllBytes($path, $payloadEmpty); $bytes += 0 }
                1 { [System.IO.File]::WriteAllBytes($path, $payloadMid); $bytes += 4096 }
                default { [System.IO.File]::WriteAllBytes($path, $payloadSmall); $bytes += 16 }
            }
            $files++
        }
    }

    $nest = $nested
    for ($n = 0; $n -lt 40; $n++) {
        $nest = Join-Path $nest ("level-{0:D2}" -f $n)
        [void][System.IO.Directory]::CreateDirectory($nest)
        $dirs++
        $leaf = Join-Path $nest "note.txt"
        [System.IO.File]::WriteAllBytes($leaf, $payloadSmall)
        $files++
        $bytes += 16
    }

    $deepPath = $deep
    for ($n = 0; $n -lt 32; $n++) {
        $deepPath = Join-Path $deepPath "d"
        [void][System.IO.Directory]::CreateDirectory($deepPath)
        $dirs++
    }
    [System.IO.File]::WriteAllBytes((Join-Path $deepPath "leaf.bin"), $payloadSmall)
    $files++
    $bytes += 16

    foreach ($name in @("alpha.txt", "beta-文件.bin", "gamma empty")) {
        $path = Join-Path $unicode $name
        [System.IO.File]::WriteAllBytes($path, $payloadSmall)
        $files++
        $bytes += 16
    }

    $pad = 0
    while (($files + $dirs) -lt $TargetEntries) {
        $path = Join-Path $wideDirs[0] ("pad-{0:D6}.dat" -f $pad)
        [System.IO.File]::WriteAllBytes($path, $payloadSmall)
        $files++
        $bytes += 16
        $pad++
    }

    $entries = $files + $dirs
    Write-Host "Fixture ready: entries=$entries files=$files dirs=$dirs bytes=$bytes"
    return [pscustomobject]@{
        entries = $entries
        files = $files
        directories = $dirs
        logicalBytes = $bytes
        root = $Root
    }
}

function Invoke-CliJson([string[]]$CliArgs) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $CliPath
    foreach ($a in $CliArgs) { [void]$psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$proc.Start()
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    $sw.Stop()
    return [pscustomobject]@{
        ms = [int]$sw.ElapsedMilliseconds
        exitCode = $proc.ExitCode
        stdout = $stdout
        stderr = $stderr
        peakWorkingSetMb = $null
    }
}

$os = Get-CimInstance Win32_OperatingSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$cs = Get-CimInstance Win32_ComputerSystem

$meta = [ordered]@{
    schema = "spacelens.stress.v01"
    timestampUtc = [DateTime]::UtcNow.ToString("o")
    environment = [ordered]@{
        os = $os.Caption + " " + $os.Version
        cpu = $cpu.Name.Trim()
        ramGiB = [math]::Round($cs.TotalPhysicalMemory / 1GB, 1)
        filesystem = "NTFS"
        build = "Release"
        cli = "build-release/cli/spacelens.exe"
        cache = "likely warm after fixture generation and first scan; first timed scan may be mixed"
        repetitions = $QueryRepeats
        note = "Single-machine generated fixture. Not a competitive claim."
    }
}

$created = New-StressFixture $fixture $Entries
$meta.fixture = [ordered]@{
    root = "<temp>/spacelens-stress-*/fixture"
    requestedEntries = $Entries
    entries = $created.entries
    files = $created.files
    directories = $created.directories
    logicalBytes = $created.logicalBytes
}

if ($created.entries -lt $Entries) {
    Write-Error "Fixture produced $($created.entries) entries; wanted >= $Entries"
}

Write-Host "scan..."
$scan = Invoke-CliJson @("scan", $fixture, "--json")
if ($scan.exitCode -ne 0) {
    Write-Error "scan failed ($($scan.exitCode)): $($scan.stderr)"
}
$meta.scan = [ordered]@{ elapsedMs = $scan.ms; exitCode = $scan.exitCode }

Write-Host "index..."
$index = Invoke-CliJson @("index", $fixture, "--json")
if ($index.exitCode -ne 0) {
    Write-Error "index failed ($($index.exitCode)): $($index.stderr)"
}
$meta.index = [ordered]@{ elapsedMs = $index.ms; exitCode = $index.exitCode }

$indexDb = $null
try {
    $indexJson = $index.stdout | ConvertFrom-Json
    if ($indexJson.elapsed_ms) { $meta.index.engineElapsedMs = [int64]$indexJson.elapsed_ms }
    if ($indexJson.index -and $indexJson.index.path) { $indexDb = $indexJson.index.path }
} catch { }

if ($indexDb -and (Test-Path $indexDb)) {
    $meta.index.dbBytes = (Get-Item $indexDb).Length
} else {
    $meta.index.dbBytes = $null
    $meta.index.dbNote = "index DB path not parsed from CLI JSON"
}

function Measure-Query([string[]]$QueryArgs, [int]$RepeatCount) {
    $samples = @()
    for ($i = 0; $i -lt $RepeatCount; $i++) {
        $r = Invoke-CliJson $QueryArgs
        if ($r.exitCode -ne 0) {
            Write-Error "query failed ($($r.exitCode)) $($QueryArgs -join ' '): $($r.stderr)"
        }
        $samples += $r.ms
    }
    $sorted = $samples | Sort-Object
    $p50 = $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.5)]
    $p95 = $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.95)]
    return [ordered]@{
        queryArgs = ($QueryArgs -join " ")
        samples = $samples
        sampleCount = $samples.Count
        minMs = ($samples | Measure-Object -Minimum).Minimum
        maxMs = ($samples | Measure-Object -Maximum).Maximum
        p50Ms = $p50
        p95Ms = $p95
        warm = $true
    }
}

Write-Host "queries ($QueryRepeats repeats each)..."
$meta.queries = @(
    (Measure-Query @("query", $fixture, "--files", "--limit", "20", "--json") $QueryRepeats),
    (Measure-Query @("query", $fixture, "--dirs", "--limit", "20", "--json") $QueryRepeats),
    (Measure-Query @("query", $fixture, "--files", "--ext", "dat", "--limit", "20", "--json") $QueryRepeats)
)

Write-Host "cancel scan..."
$meta.cancellation = [ordered]@{
    attempted = $true
    jobState = "not_started"
    note = "Best-effort process stop shortly after start; not a correctness proof of cooperative cancel."
}
try {
    $cancelJob = Start-Job -ScriptBlock {
        param($exe, $path)
        & $exe scan $path --json
        return $LASTEXITCODE
    } -ArgumentList $CliPath, $fixture
    Start-Sleep -Milliseconds 80
    Stop-Job -Job $cancelJob -ErrorAction SilentlyContinue
    $meta.cancellation.jobState = [string]$cancelJob.State
    Remove-Job -Job $cancelJob -Force -ErrorAction SilentlyContinue
} catch {
    $meta.cancellation.jobState = "error"
    $meta.cancellation.error = [string]$_.Exception.Message
}

$json = $meta | ConvertTo-Json -Depth 8
$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($ReportPath, $json, $utf8)
Write-Host "Report: $ReportPath"
Write-Host ($json)

if (-not $KeepFixture) {
    Remove-Item -Recurse -Force $WorkRoot -ErrorAction SilentlyContinue
}

if ($created.entries -lt 100000 -and $Entries -ge 100000) {
    exit 2
}
exit 0
