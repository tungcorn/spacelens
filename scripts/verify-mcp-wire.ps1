# Dual-era MCP wire gate: discover, initialize, tools, parse errors, stdout hygiene.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$McpPath
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $McpPath)) {
    Write-Error "MCP server not found: $McpPath"
}
$exe = (Resolve-Path $McpPath).Path
Write-Host "Verifying MCP wire: $exe"

function Invoke-McpBatch {
    param([string[]]$Lines)
    $inputText = ($Lines -join "`n") + "`n"
    $raw = $inputText | & $exe
    return [pscustomobject]@{
        Lines = @($raw)
        Raw   = [string]$raw
    }
}

$meta = '"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{},"io.modelcontextprotocol/clientInfo":{"name":"wire","version":"1"}}'
$discover = "{`"jsonrpc`":`"2.0`",`"id`":`"d1`",`"method`":`"server/discover`",`"params`":{$meta}}"
$init = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"wire","version":"1"}}}'
$listModern = "{`"jsonrpc`":`"2.0`",`"id`":2,`"method`":`"tools/list`",`"params`":{$meta}}"
$caps = "{`"jsonrpc`":`"2.0`",`"id`":3,`"method`":`"tools/call`",`"params`":{`"name`":`"storage_capabilities`",`"arguments`":{},$meta}}"
$bad = "{not-json"
$unsupported = '{"jsonrpc":"2.0","id":"v","method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"1999-01-01","io.modelcontextprotocol/clientCapabilities":{}}}}'

$batch = Invoke-McpBatch -Lines @($discover, $init, $listModern, $caps, $bad, $unsupported)
if ($batch.Lines.Count -ne 6) {
    Write-Error "expected 6 responses, got $($batch.Lines.Count)`n$($batch.Raw)"
}

foreach ($line in $batch.Lines) {
    if ($line -match "`n") {
        Write-Error "protocol line contained an embedded newline"
    }
    $null = $line | ConvertFrom-Json
}

$discoverJson = $batch.Lines[0] | ConvertFrom-Json
if ($discoverJson.result.resultType -ne "complete") {
    Write-Error "discover missing resultType=complete"
}
if ($discoverJson.result.supportedVersions -notcontains "2026-07-28") {
    Write-Error "discover missing 2026-07-28"
}
if ($discoverJson.result.supportedVersions -notcontains "2025-11-25") {
    Write-Error "discover missing 2025-11-25"
}
if ($null -eq $discoverJson.result.capabilities.tools) {
    Write-Error "discover must advertise tools"
}
if ($null -ne $discoverJson.result.capabilities.resources) {
    Write-Error "discover must not advertise resources"
}

$initJson = $batch.Lines[1] | ConvertFrom-Json
if ($initJson.result.protocolVersion -ne "2025-11-25") {
    Write-Error "initialize did not negotiate 2025-11-25"
}

$listJson = $batch.Lines[2] | ConvertFrom-Json
if ($listJson.result.tools.Count -ne 6) {
    Write-Error "tools/list count"
}
if ($null -eq $listJson.result.ttlMs) {
    Write-Error "modern tools/list missing ttlMs"
}

$capsJson = $batch.Lines[3] | ConvertFrom-Json
if ($capsJson.result.isError -eq $true) {
    Write-Error "storage_capabilities isError"
}
if ($capsJson.result.structuredContent.read_only -ne $true) {
    Write-Error "structuredContent.read_only"
}

$parseJson = $batch.Lines[4] | ConvertFrom-Json
if ($parseJson.error.code -ne -32700) {
    Write-Error "invalid JSON must be -32700"
}

$verJson = $batch.Lines[5] | ConvertFrom-Json
if ($verJson.error.code -ne -32022) {
    Write-Error "unsupported version must be -32022"
}

# Stdout hygiene: a process that prints a banner would fail ConvertFrom-Json above.
Write-Host "MCP wire: discover + initialize + tools + parse/version errors"
