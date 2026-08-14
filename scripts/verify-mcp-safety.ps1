# Safety gate for spacelens-mcp.exe: read-only tools only, no Qt, no mutation verbs.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$McpPath,
    [string]$ExpectedVersion = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $McpPath)) {
    Write-Error "MCP server not found: $McpPath"
}
$exe = (Resolve-Path $McpPath).Path
Write-Host "Verifying MCP safety: $exe"

function Invoke-McpBatch {
    param([string[]]$Lines)
    $inputText = ($Lines -join "`n") + "`n"
    $raw = $inputText | & $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Error "spacelens-mcp exited $LASTEXITCODE`n$raw"
    }
    return @($raw)
}

$init = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"safety","version":"1"}}}'
$list = '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
$caps = '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"storage_capabilities","arguments":{}}}'
$refresh = '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"index_refresh","arguments":{"path":"C:\\"}}}'
$delete = '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"storage_delete","arguments":{"path":"C:\\"}}}'

$lines = Invoke-McpBatch -Lines @($init, $list, $caps, $refresh, $delete)
if ($lines.Count -lt 5) {
    Write-Error "expected 5 protocol responses, got $($lines.Count): $lines"
}

$initJson = $lines[0] | ConvertFrom-Json
if ($ExpectedVersion) {
    $got = [string]$initJson.result.serverInfo.version
    if ($got -ne $ExpectedVersion) {
        Write-Error "MCP serverInfo.version '$got' != $ExpectedVersion"
    }
}

$listJson = $lines[1] | ConvertFrom-Json
$names = @($listJson.result.tools | ForEach-Object { $_.name })
$allowed = @(
    "storage_capabilities",
    "storage_overview",
    "storage_opportunities",
    "storage_query",
    "storage_duplicates",
    "storage_index_status"
)
if ($names.Count -ne $allowed.Count) {
    Write-Error "unexpected tool count $($names.Count): $($names -join ', ')"
}
foreach ($name in $names) {
    if ($allowed -notcontains $name) {
        Write-Error "unexpected tool: $name"
    }
    if ($name -match "delete|recycle|restore|refresh|execute|move|maintenance") {
        Write-Error "mutation-like tool advertised: $name"
    }
}
foreach ($tool in @($listJson.result.tools)) {
    if ($tool.annotations.readOnlyHint -ne $true) {
        Write-Error "$($tool.name) readOnlyHint is not true"
    }
    if ($tool.annotations.destructiveHint -eq $true) {
        Write-Error "$($tool.name) destructiveHint is true"
    }
}

$capsJson = $lines[2] | ConvertFrom-Json
$structured = $capsJson.result.structuredContent
if ($structured.filesystem_mutation -ne $false) {
    Write-Error "capabilities filesystem_mutation is not false"
}
if ($structured.read_only -ne $true) {
    Write-Error "capabilities read_only is not true"
}
if ($structured.features.filesystem_mutation -ne $false) {
    Write-Error "features.filesystem_mutation is not false"
}
if ($structured.features.index_refresh -ne $false) {
    Write-Error "features.index_refresh is not false"
}
if ($structured.features.embedded_model -ne $false) {
    Write-Error "features.embedded_model is not false"
}

$refreshJson = $lines[3] | ConvertFrom-Json
if ($null -eq $refreshJson.error -or $refreshJson.error.code -ne -32602) {
    Write-Error "index_refresh must be unknown (-32602)"
}
$deleteJson = $lines[4] | ConvertFrom-Json
if ($null -eq $deleteJson.error -or $deleteJson.error.code -ne -32602) {
    Write-Error "storage_delete must be unknown (-32602)"
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($dumpbin) {
    $deps = & dumpbin.exe /DEPENDENTS $exe 2>$null | Out-String
    if ($deps -match "Qt6") {
        Write-Error "spacelens-mcp links Qt:`n$deps"
    }
} else {
    Write-Host "dumpbin.exe not on PATH; skipping Qt dependent check"
}

Write-Host "MCP safety: 6 read-only tools, mutation=false, refresh/delete unknown"
