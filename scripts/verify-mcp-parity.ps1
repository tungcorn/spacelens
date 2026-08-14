# CLI vs MCP semantic parity on a temporary fixture. Never scans user data.

[CmdletBinding()]
param(
    [string]$CliPath = "",
    [string]$McpPath = "",
    [switch]$KeepFixture
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Resolve-Tool {
    param([string]$Given, [string[]]$Candidates, [string]$Label)
    if ($Given -and (Test-Path $Given)) {
        return (Resolve-Path $Given).Path
    }
    foreach ($candidate in $Candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }
    Write-Error "$Label not found. Pass -$Label."
}

$cli = Resolve-Tool -Given $CliPath -Label "CliPath" -Candidates @(
    (Join-Path $root "build-debug\cli\spacelens.exe"),
    (Join-Path $root "build-release\cli\spacelens.exe"),
    (Join-Path $root "build-cli-release\cli\spacelens.exe")
)
$mcp = Resolve-Tool -Given $McpPath -Label "McpPath" -Candidates @(
    (Join-Path $root "build-debug\mcp\spacelens-mcp.exe"),
    (Join-Path $root "build-release\mcp\spacelens-mcp.exe"),
    (Join-Path $root "build-cli-release\mcp\spacelens-mcp.exe")
)

Write-Host "CLI: $cli"
Write-Host "MCP: $mcp"

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
    for ($i = 0; $i -lt $buffer.Length; $i++) { $buffer[$i] = $Fill }
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try {
        $remaining = $Bytes
        while ($remaining -gt 0) {
            $chunk = [int][Math]::Min($remaining, $buffer.Length)
            $fs.Write($buffer, 0, $chunk)
            $remaining -= $chunk
        }
    } finally { $fs.Close() }
    if ($DaysAgo -ne 0) {
        [System.IO.File]::SetLastWriteTime($Path, (Get-Date).AddDays(-$DaysAgo))
    }
}

function Invoke-CliJson {
    param([string[]]$Arguments)
    $raw = & $cli @Arguments
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CLI $($Arguments -join ' ') exited $LASTEXITCODE`n$raw"
    }
    return $raw | ConvertFrom-Json
}

function Invoke-McpTool {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)]$Arguments,
        [int]$Id = 20
    )
    $payload = [ordered]@{
        jsonrpc = "2.0"
        id      = $Id
        method  = "tools/call"
        params  = [ordered]@{
            name      = $Name
            arguments = $Arguments
        }
    }
    $line = ($payload | ConvertTo-Json -Compress -Depth 12)
    $raw = ($line + "`n") | & $mcp
    if ($LASTEXITCODE -ne 0) {
        Write-Error "MCP $Name exited $LASTEXITCODE`n$raw"
    }
    $msg = $raw | ConvertFrom-Json
    if ($null -eq $msg.result) {
        Write-Error "MCP $Name had no result: $raw"
    }
    return [pscustomobject]@{
        Message    = $msg
        Structured = $msg.result.structuredContent
        Bytes      = [Text.Encoding]::UTF8.GetByteCount([string]$raw)
        Raw        = [string]$raw
    }
}

function Paths-Of {
    param($Items)
    @($Items | ForEach-Object { $_.path })
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-mcp-" + [guid]::NewGuid().ToString("N"))
$fixture = Join-Path $work "workstation"
New-Item -ItemType Directory -Force -Path $fixture | Out-Null

try {
    $app = Join-Path $fixture "Projects\app"
    Write-SizedFile -Path (Join-Path $app "node_modules\left-pad.js") -Bytes (12MB) -DaysAgo 10 -Fill 0x11
    Write-SizedFile -Path (Join-Path $app "build\CMakeFiles\generated.bin") -Bytes (11MB) -DaysAgo 5 -Fill 0x21
    Write-SizedFile -Path (Join-Path $app ".cache\tmp.dat") -Bytes (5MB) -DaysAgo 20 -Fill 0x31
    Write-SizedFile -Path (Join-Path $fixture "old-backup.zip") -Bytes (9MB) -DaysAgo 400 -Fill 0x41
    Write-SizedFile -Path (Join-Path $fixture "recent-vm.iso") -Bytes (40MB) -DaysAgo 2 -Fill 0x51

    $cliOverviewSw = [System.Diagnostics.Stopwatch]::StartNew()
    $cliOverview = Invoke-CliJson @("overview", $fixture, "--json")
    $cliOverviewSw.Stop()
    $mcpOverviewSw = [System.Diagnostics.Stopwatch]::StartNew()
    $mcpOverview = Invoke-McpTool -Name "storage_overview" -Arguments @{
        path   = $fixture
        source = "live_scan"
    }
    $mcpOverviewSw.Stop()

    if ($mcpOverview.Structured.ok -ne $true) { Write-Error "MCP overview not ok" }
    if ($mcpOverview.Structured.source -ne "live_scan") { Write-Error "MCP overview source" }
    if ($mcpOverview.Structured.summary.logical_bytes -ne $cliOverview.summary.logical_bytes) {
        Write-Error "overview logical_bytes CLI=$($cliOverview.summary.logical_bytes) MCP=$($mcpOverview.Structured.summary.logical_bytes)"
    }
    $cliFiles = Paths-Of $cliOverview.largest_files
    $mcpFiles = Paths-Of $mcpOverview.Structured.largest_files
    if (($cliFiles -join "|") -ne ($mcpFiles -join "|")) {
        Write-Error "largest_files diverge`nCLI=$cliFiles`nMCP=$mcpFiles"
    }

    $cliOpp = Invoke-CliJson @("opportunities", $fixture, "--json")
    $mcpOpp = Invoke-McpTool -Name "storage_opportunities" -Arguments @{
        path   = $fixture
        source = "live_scan"
    } -Id 21
    if ($mcpOpp.Structured.summary.unique_review_bytes -ne $cliOpp.summary.unique_review_bytes) {
        Write-Error "unique_review_bytes CLI=$($cliOpp.summary.unique_review_bytes) MCP=$($mcpOpp.Structured.summary.unique_review_bytes)"
    }
    $cliOppPaths = Paths-Of $cliOpp.opportunities
    $mcpOppPaths = Paths-Of $mcpOpp.Structured.opportunities
    if (($cliOppPaths -join "|") -ne ($mcpOppPaths -join "|")) {
        Write-Error "opportunities diverge`nCLI=$cliOppPaths`nMCP=$mcpOppPaths"
    }

    $null = Invoke-CliJson @("index", $fixture, "--json")
    $cliQuery = Invoke-CliJson @(
        "query", $fixture, "--dirs", "--under", (Join-Path $fixture "Projects\app"), "--limit", "20", "--json"
    )
    $mcpQuery = Invoke-McpTool -Name "storage_query" -Arguments @{
        path        = $fixture
        object_type = "directory"
        under       = (Join-Path $fixture "Projects\app")
        limit       = 20
    } -Id 22
    if ($mcpQuery.Message.result.isError -eq $true) {
        Write-Error "MCP query isError: $($mcpQuery.Raw)"
    }
    if ($mcpQuery.Structured.returned_items -ne $cliQuery.returned_items) {
        Write-Error "query returned_items CLI=$($cliQuery.returned_items) MCP=$($mcpQuery.Structured.returned_items)"
    }
    $cliQ = Paths-Of $cliQuery.results
    $mcpQ = Paths-Of $mcpQuery.Structured.results
    if (($cliQ -join "|") -ne ($mcpQ -join "|")) {
        Write-Error "query results diverge`nCLI=$cliQ`nMCP=$mcpQ"
    }

    $status = Invoke-McpTool -Name "storage_index_status" -Arguments @{ path = $fixture } -Id 23
    if ($status.Structured.ok -ne $true) { Write-Error "index status not ok" }

    Write-Host ("parity overview CLI={0}ms MCP={1}ms json={2} bytes" -f `
            $cliOverviewSw.ElapsedMilliseconds, $mcpOverviewSw.ElapsedMilliseconds, $mcpOverview.Bytes)
    Write-Host "MCP parity: overview/opportunities/query match CLI on temp fixture"
}
finally {
    if (-not $KeepFixture) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host "Kept fixture: $work"
    }
}
