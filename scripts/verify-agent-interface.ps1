# Acceptance gate for Storage Intelligence / Agent Interface V1.
# Generates a temporary developer-workstation fixture and runs the real CLI.
# Never points at the source tree or user data.

[CmdletBinding()]
param(
    [string]$CliPath = "",
    [switch]$KeepFixture
)

$ErrorActionPreference = "Stop"
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
Write-Host "Agent interface gate: $exe"

function Write-SizedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int64]$Bytes,
        [int]$DaysAgo = 0,
        [byte]$Fill = 0
    )
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $buffer = New-Object byte[] ([Math]::Min($Bytes, 1MB))
    for ($i = 0; $i -lt $buffer.Length; $i++) {
        $buffer[$i] = $Fill
    }
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
        $stamp = (Get-Date).AddDays(-$DaysAgo)
        [System.IO.File]::SetLastWriteTime($Path, $stamp)
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
        Write-Error "spacelens $($Arguments -join ' ') exited $code (accepted: $($AcceptExit -join ','))`n$raw"
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

function Has-GroupId {
    param($Groups, [string]$Id)
    foreach ($g in @($Groups)) {
        if ($g.id -eq $Id) {
            return $true
        }
    }
    return $false
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-agent-" + [guid]::NewGuid().ToString("N"))
$fixture = Join-Path $work "workstation"
New-Item -ItemType Directory -Force -Path $fixture | Out-Null

try {
    $app = Join-Path $fixture "Projects\app"
    Write-SizedFile -Path (Join-Path $app "node_modules\left-pad.js") -Bytes (12MB) -DaysAgo 10 -Fill 0x11
    Write-SizedFile -Path (Join-Path $app "node_modules\lodash.js") -Bytes (8MB) -DaysAgo 10 -Fill 0x12
    Write-SizedFile -Path (Join-Path $app "build\CMakeFiles\generated.bin") -Bytes (11MB) -DaysAgo 5 -Fill 0x21
    Write-SizedFile -Path (Join-Path $app "build\CMakeCache.txt") -Bytes 4000 -DaysAgo 5 -Fill 0x22
    Write-SizedFile -Path (Join-Path $app ".cache\tmp.dat") -Bytes (5MB) -DaysAgo 20 -Fill 0x31
    Write-SizedFile -Path (Join-Path $fixture "old-backup.zip") -Bytes (9MB) -DaysAgo 400 -Fill 0x41
    Write-SizedFile -Path (Join-Path $fixture "recent-vm.iso") -Bytes (40MB) -DaysAgo 2 -Fill 0x51
    Write-SizedFile -Path (Join-Path $fixture "holiday.jpg") -Bytes (3MB) -DaysAgo 30 -Fill 0x61
    Write-SizedFile -Path (Join-Path $fixture "old-setup.msi") -Bytes (8MB) -DaysAgo 400 -Fill 0x71
    Write-SizedFile -Path (Join-Path $fixture "rust-app\Cargo.toml") -Bytes 200 -DaysAgo 20 -Fill 0x81
    Write-SizedFile -Path (Join-Path $fixture "rust-app\target\app.exe") -Bytes (14MB) -DaysAgo 8 -Fill 0x82
    Write-SizedFile -Path (Join-Path $fixture "dotnet-app\App.csproj") -Bytes 300 -DaysAgo 20 -Fill 0x83
    Write-SizedFile -Path (Join-Path $fixture "dotnet-app\bin\App.dll") -Bytes (12MB) -DaysAgo 6 -Fill 0x84
    Write-SizedFile -Path (Join-Path $fixture "py-app\pyproject.toml") -Bytes 120 -DaysAgo 20 -Fill 0x85
    Write-SizedFile -Path (Join-Path $fixture "py-app\.venv\pyvenv.cfg") -Bytes 80 -DaysAgo 12 -Fill 0x86
    Write-SizedFile -Path (Join-Path $fixture "py-app\.venv\lib.bin") -Bytes (11MB) -DaysAgo 12 -Fill 0x87
    Write-SizedFile -Path (Join-Path $fixture "Photos\build\IMG_001.jpg") -Bytes (12MB) -DaysAgo 400 -Fill 0x91

    $dupA = Join-Path $fixture "copies\report-a.bin"
    $dupB = Join-Path $fixture "copies\report-b.bin"
    Write-SizedFile -Path $dupA -Bytes (2MB) -DaysAgo 120 -Fill 0xAB
    Copy-Item -LiteralPath $dupA -Destination $dupB -Force
    [System.IO.File]::SetLastWriteTime($dupB, (Get-Date).AddDays(-120))

    $linkSrc = Join-Path $fixture "links\orig.bin"
    $linkDst = Join-Path $fixture "links\alias.bin"
    Write-SizedFile -Path $linkSrc -Bytes (2MB) -DaysAgo 150 -Fill 0xCD
    New-Item -ItemType HardLink -Path $linkDst -Target $linkSrc | Out-Null

    # --- capabilities ---
    $caps = Invoke-CliJson -Arguments @("capabilities", "--json")
    Assert-True ($caps.Json.filesystem_mutation -eq $false) "capabilities filesystem_mutation is not false"
    Assert-True ($caps.Json.read_only -eq $true) "capabilities read_only is not true"
    Assert-True ($caps.Json.commands -contains "overview") "capabilities missing overview"
    Assert-True ($caps.Json.commands -contains "opportunities") "capabilities missing opportunities"
    Assert-True ($caps.Json.features.storage_overview -eq $true) "features.storage_overview missing"
    Assert-True ($caps.Json.features.storage_opportunities -eq $true) "features.storage_opportunities missing"
    Assert-True ($caps.Json.features.filesystem_mutation -eq $false) "features.filesystem_mutation is not false"
    Write-Host "A0 capabilities: overview/opportunities advertised, mutation=false"

    # --- A: what consumes space ---
    $overviewSw = [System.Diagnostics.Stopwatch]::StartNew()
    $overview = Invoke-CliJson -Arguments @("overview", $fixture, "--json")
    $overviewSw.Stop()
    Assert-True ($overview.Json.ok -eq $true) "overview not ok"
    Assert-True ($overview.Json.command -eq "overview") "overview command field"
    Assert-True ($overview.Json.source -eq "live_scan") "overview source must be live_scan"
    Assert-True ($overview.Json.filesystem_mutation -eq $false) "overview mutation"
    Assert-True ($overview.Json.summary.logical_bytes -gt 0) "overview logical_bytes"
    Assert-True (Has-PathNeedle $overview.Json.largest_files "recent-vm.iso") "overview must list recent-vm.iso"
    foreach ($dir in @($overview.Json.largest_directories)) {
        Assert-True ($dir.path -ne $overview.Json.root) "overview listed the root as a consumer"
    }
    Assert-True ($overview.Raw -notmatch "safe_to_delete") "overview leaked safe_to_delete"
    $overviewBytes = [Text.Encoding]::UTF8.GetByteCount($overview.Raw)
    Assert-True ($overview.Json.largest_directories.Count -le 10) "overview dirs unbounded"
    Write-Host ("A overview: {0} ms, {1} bytes JSON, logical={2}" -f `
            $overviewSw.ElapsedMilliseconds, $overviewBytes, $overview.Json.summary.logical_bytes)

    $scanSw = [System.Diagnostics.Stopwatch]::StartNew()
    $null = Invoke-CliJson -Arguments @("scan", $fixture, "--json")
    $scanSw.Stop()
    $topDirsSw = [System.Diagnostics.Stopwatch]::StartNew()
    $null = Invoke-CliJson -Arguments @("top", $fixture, "--dirs", "--json")
    $topDirsSw.Stop()
    $topFilesSw = [System.Diagnostics.Stopwatch]::StartNew()
    $null = Invoke-CliJson -Arguments @("top", $fixture, "--files", "--json")
    $topFilesSw.Stop()
    $separateMs = $scanSw.ElapsedMilliseconds + $topDirsSw.ElapsedMilliseconds + $topFilesSw.ElapsedMilliseconds
    Write-Host ("A timing: overview={0}ms vs scan+top-dirs+top-files={1}ms" -f `
            $overviewSw.ElapsedMilliseconds, $separateMs)

    # --- B: strongest review opportunities ---
    $opp = Invoke-CliJson -Arguments @("opportunities", $fixture, "--json")
    Assert-True ($opp.Json.ok -eq $true) "opportunities not ok"
    Assert-True ($opp.Json.command -eq "opportunities") "opportunities command"
    Assert-True ($opp.Json.source -eq "live_scan") "opportunities source"
    Assert-True ($opp.Json.planning_only -eq $true) "planning_only"
    Assert-True ($opp.Json.read_only -eq $true) "opportunities read_only"
    Assert-True ($opp.Json.filesystem_mutation -eq $false) "opportunities mutation"
    Assert-True ($opp.Raw -notmatch "safe_to_delete") "opportunities leaked safe_to_delete"
    Assert-True ($opp.Raw -notmatch "potential_reclaim_bytes") "opportunities leaked potential_reclaim_bytes"
    Assert-True ($null -ne $opp.Json.summary.unique_review_bytes) "unique_review_bytes missing"
    Assert-True ($opp.Json.ranking_policy -eq "opportunity_rank_v2") "ranking_policy must be opportunity_rank_v2"
    Assert-True (Has-PathNeedle $opp.Json.opportunities "node_modules") "missing node_modules opportunity"
    Assert-True (Has-PathNeedle $opp.Json.opportunities "build") "missing build opportunity"
    Assert-True (Has-PathNeedle $opp.Json.opportunities "old-backup.zip") "missing old zip opportunity"
    Assert-True (Has-PathNeedle $opp.Json.opportunities "target") "missing rust target opportunity"
    Assert-True (Has-PathNeedle $opp.Json.opportunities ".venv") "missing python venv opportunity"
    Assert-True (Has-PathNeedle $opp.Json.opportunities "old-setup.msi") "missing old installer opportunity"
    Assert-True (-not (Has-PathNeedle $opp.Json.opportunities "recent-vm.iso")) "recent VM must not be an opportunity"
    Assert-True (-not (Has-PathNeedle $opp.Json.opportunities "holiday.jpg")) "recent photo must not be an opportunity"
    foreach ($item in @($opp.Json.opportunities)) {
        if ($null -ne $item.path -and ($item.path -like "*Photos*build*")) {
            Assert-True ($item.classification -ne "BuildArtifact") `
                "Photos\build must not be generated output"
        }
        Assert-True ($null -ne $item.evidence) "opportunity missing evidence object"
    }
    Assert-True ($opp.Json.summary.returned_count -le 20) "opportunities unbounded"
    $oppBytes = [Text.Encoding]::UTF8.GetByteCount($opp.Raw)
    Assert-True ($oppBytes -lt 256KB) "opportunities JSON too large for an agent ($oppBytes)"
    $ranks = @($opp.Json.opportunities | ForEach-Object { $_.opportunity_rank })
    for ($i = 0; $i -lt $ranks.Count; $i++) {
        Assert-True ($ranks[$i] -eq ($i + 1)) "opportunity_rank not sequential"
    }
    Write-Host ("B opportunities: {0} items, unique_review_bytes={1}, json={2} bytes" -f `
            $opp.Json.summary.returned_count, $opp.Json.summary.unique_review_bytes, $oppBytes)

    # --- C: developer / generated / cache groups ---
    Assert-True (Has-GroupId $opp.Json.groups "developer_dependencies") "missing developer_dependencies group"
    Assert-True (Has-GroupId $opp.Json.groups "generated_outputs") "missing generated_outputs group"
    Assert-True (Has-GroupId $opp.Json.groups "temporary_data") "missing temporary_data group"
    foreach ($g in @($opp.Json.groups)) {
        Assert-True ($null -ne $g.strongest_candidate_strength) "group missing strongest_candidate_strength"
    }
    Assert-True ($null -ne $overview.Json.opportunity_summary) "overview missing opportunity_summary"

    $onlyBuild = Invoke-CliJson -Arguments @("opportunities", $fixture, "--classification", "BuildArtifact", "--json")
    Assert-True (Has-PathNeedle $onlyBuild.Json.opportunities "build") "classification filter missed cmake build"
    Assert-True (-not (Has-PathNeedle $onlyBuild.Json.opportunities ".venv")) "classification filter leaked venv"
    Write-Host "C groups: $((@($opp.Json.groups) | ForEach-Object { $_.id }) -join ', ')"

    # --- nested overlap ---
    $cmake = @($opp.Json.opportunities | Where-Object { $_.path -like "*CMakeFiles*" })
    if ($cmake.Count -gt 0) {
        Assert-True ($cmake[0].overlapped -eq $true) "CMakeFiles should be overlapped by build"
        $uniqueSum = 0L
        foreach ($item in @($opp.Json.opportunities)) {
            if (-not $item.overlapped) {
                $uniqueSum += [int64]$item.logical_bytes
            }
        }
        Assert-True ($uniqueSum -eq [int64]$opp.Json.summary.unique_review_bytes) `
            "unique_review_bytes does not match non-overlapped sum"
        Write-Host "C overlap: CMakeFiles overlapped=true, unique bytes not double-counted"
    } else {
        Write-Host "C overlap: CMakeFiles not returned (still covered by build group)"
    }

    # --- D: old large files ---
    $old = Invoke-CliJson -Arguments @("find", $fixture, "--min-size", "1MB", "--older-than", "90", "--json")
    Assert-True ($old.Json.source -eq "live_scan") "find source"
    Assert-True (Has-PathNeedle $old.Json.results "old-backup.zip") "find missed old zip"
    Assert-True (-not (Has-PathNeedle $old.Json.results "recent-vm.iso")) "find included recent VM"
    $sample = @($old.Json.results)[0]
    Assert-True ($null -ne $sample.classification) "find missing classification"
    Assert-True ($null -ne $sample.location_safety) "find missing location_safety"
    Assert-True ($null -ne $sample.reclaimability) "find missing reclaimability"
    Assert-True ($null -ne $sample.candidate_strength) "find missing candidate_strength"
    Write-Host "D find: old zip present, analysis fields present"

    # --- missing index is exit 6 ---
    $empty = Join-Path $work "empty-no-index"
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    $missing = Invoke-CliJson -Arguments @("opportunities", $empty, "--from-index", "--json") -AcceptExit @(6)
    Assert-True ($missing.Code -eq 6) "missing index must be exit 6"
    Write-Host "F index-missing: exit 6"

    # --- index + indexed workflow + duplicates ---
    $idx = Invoke-CliJson -Arguments @("index", $fixture, "--json")
    Assert-True ($idx.Json.ok -eq $true) "index failed: $($idx.Raw)"

    $idxOverview = Invoke-CliJson -Arguments @("overview", $fixture, "--from-index", "--json")
    Assert-True ($idxOverview.Json.source -eq "persistent_index") "indexed overview source"
    Assert-True ($idxOverview.Json.ok -eq $true) "indexed overview not ok"
    Assert-True (Has-PathNeedle $idxOverview.Json.largest_files "recent-vm.iso") "indexed overview missed VM"
    Assert-True ($null -ne $idxOverview.Json.index.freshness) "indexed overview missing index.freshness"
    Assert-True ($idxOverview.Json.index.freshness.basis -eq "published_snapshot") "overview freshness basis"
    Assert-True ($idxOverview.Raw -notmatch '"fresh"\s*:') "indexed overview leaked bare fresh"

    $idxOpp = Invoke-CliJson -Arguments @("opportunities", $fixture, "--from-index", "--json")
    Assert-True ($idxOpp.Json.source -eq "persistent_index") "indexed opportunities source"
    Assert-True (Has-PathNeedle $idxOpp.Json.opportunities "node_modules") "indexed opportunities missed node_modules"
    Assert-True (-not (Has-PathNeedle $idxOpp.Json.opportunities "recent-vm.iso")) "indexed opportunities included VM"

    $under = Join-Path $app "node_modules"
    $query = Invoke-CliJson -Arguments @("query", $fixture, "--files", "--under", $under, "--limit", "20", "--json")
    Assert-True ($query.Json.source -eq "persistent_index") "query source"
    Assert-True ($query.Json.ok -eq $true) "query --under failed"
    Assert-True ($null -ne $query.Json.index.freshness) "query missing index.freshness"
    Assert-True ($query.Json.index.freshness.basis -eq "published_snapshot") "query freshness basis"
    Assert-True ($query.Raw -notmatch '"fresh"\s*:') "query leaked bare fresh"
    Assert-True ($query.Json.returned_items -gt 0) "query --under returned no files"
    foreach ($hit in @($query.Json.results)) {
        Assert-True ($hit.path -like "$under*") "query --under leaked $($hit.path)"
    }
    Write-Host "F drill-down: query --under returned $($query.Json.returned_items) hits"

    $dups = Invoke-CliJson -Arguments @("duplicates", $fixture, "--min-size", "1MB", "--json")
    Assert-True ($dups.Json.filesystem_mutation -eq $false) "duplicates mutation"
    Assert-True ($dups.Json.planning_only -eq $true) "duplicates planning_only"
    $independent = $false
    $hardlinkZero = $false
    foreach ($group in @($dups.Json.groups)) {
        $paths = @()
        foreach ($instance in @($group.instances)) {
            foreach ($p in @($instance.paths)) {
                $paths += [string]$p.path
            }
        }
        $joined = $paths -join "|"
        if ($joined -match "report-a" -and $joined -match "report-b") {
            $independent = $true
            Assert-True ([int64]$group.potential_redundant_logical_bytes -eq 2MB) `
                "independent copies should report one extra 2MB identity"
        }
        if ($joined -match "orig.bin" -and $joined -match "alias.bin") {
            $hardlinkZero = $true
            Assert-True ([int64]$group.potential_redundant_logical_bytes -eq 0) `
                "hardlink-only group must report 0 redundant logical bytes"
        }
    }
    Assert-True ($independent) "duplicates missed independent copy pair"
    Assert-True ($hardlinkZero -or [int64]$dups.Json.summary.hardlink_alias_paths -ge 1) `
        "hardlink pair was not represented as aliases"
    Assert-True ([int64]$dups.Json.summary.potential_redundant_logical_bytes -ge 2MB) `
        "summary redundant bytes should include the independent copies"
    Write-Host ("E duplicates: redundant_logical={0} hardlink_aliases={1}" -f `
            $dups.Json.summary.potential_redundant_logical_bytes,
            $dups.Json.summary.hardlink_alias_paths)

    $null = & $exe overview $fixture --max-index-age-seconds 3600 --json 2>&1
    Assert-True ($LASTEXITCODE -eq 2) "max-age without --from-index must be usage"

    $freshOk = Invoke-CliJson -Arguments @(
        "overview", $fixture, "--from-index", "--max-index-age-seconds", "86400", "--json"
    )
    Assert-True ($freshOk.Json.ok -eq $true) "generous max-age should pass"
    Assert-True ($freshOk.Json.index.freshness.max_age_satisfied -eq $true) "max_age_satisfied"

    $ageSec = $idxOverview.Json.index.freshness.age_seconds
    if ($null -ne $ageSec -and [int64]$ageSec -gt 0) {
        $stale = Invoke-CliJson -Arguments @(
            "overview", $fixture, "--from-index", "--max-index-age-seconds", "0", "--json"
        ) -AcceptExit @(4)
        Assert-True ($stale.Code -eq 4) "too-old must be exit 4"
        Assert-True ($stale.Json.ok -eq $false) "too-old ok flag"
        Assert-True ($stale.Json.error -eq "index_too_old") "too-old error code"
        Assert-True ($stale.Json.index.freshness.max_age_satisfied -eq $false) "too-old max_age_satisfied"
        Assert-True ($stale.Raw -match "No refresh was performed" -or
                     $stale.Json.error -eq "index_too_old") "too-old must not refresh"
    }
    Write-Host "H freshness: index.freshness present, max-age fail-closed"

    # --- G: delete unavailable ---
    $deleteOut = & $exe delete $fixture 2>&1 | Out-String
    $deleteCode = $LASTEXITCODE
    Assert-True ($deleteCode -ne 0) "delete was accepted (exit 0)"
    Assert-True ($deleteOut -notmatch '(?i)recycled|deleted files|moved to') "delete looked like a mutation"
    $keepOut = & $exe duplicates $fixture --keep-one 2>&1 | Out-String
    Assert-True ($LASTEXITCODE -ne 0) "duplicates --keep-one accepted"
    Write-Host "G mutation verbs rejected (delete exit $deleteCode)"

    Write-Host "STORAGE_INTELLIGENCE_AGENT_V1 script PASS"
    exit 0
} finally {
    if (-not $KeepFixture) {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    } else {
        Write-Host "Kept fixture: $work"
    }
}
