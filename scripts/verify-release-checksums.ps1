# Validate a SHA256SUMS.txt against a set of attached zip names.
# Internal/workflow manifests may list both zips. A published Release
# manifest must name exactly the attached archives.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SumsPath,
    [Parameter(Mandatory = $true)]
    [string[]]$AttachedNames,
    [string]$ZipDir = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $SumsPath)) {
    Write-Error "checksum file not found: $SumsPath"
}
if (-not $ZipDir) {
    $ZipDir = Split-Path -Parent (Resolve-Path $SumsPath)
}

$attached = @($AttachedNames | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($attached.Count -eq 0) {
    Write-Error "AttachedNames is empty"
}

$parsed = @{}
foreach ($raw in Get-Content $SumsPath) {
    $line = $raw.Trim()
    if ($line.Length -eq 0 -or $line.StartsWith("#")) { continue }
    if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') {
        Write-Error "malformed checksum line: $line"
    }
    $name = $Matches[2].Trim()
    if ($parsed.ContainsKey($name)) {
        Write-Error "duplicate checksum entry: $name"
    }
    $parsed[$name] = $Matches[1].ToLowerInvariant()
}

foreach ($name in $parsed.Keys) {
    if ($attached -notcontains $name) {
        Write-Error "checksum names unpublished asset: $name"
    }
}
foreach ($name in $attached) {
    if (-not $parsed.ContainsKey($name)) {
        Write-Error "attached asset missing from checksums: $name"
    }
}

foreach ($name in $attached) {
    $path = Join-Path $ZipDir $name
    if (-not (Test-Path $path)) {
        Write-Error "attached zip not found: $path"
    }
    $actual = (Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant()
    if ($actual -ne $parsed[$name]) {
        Write-Error "SHA-256 mismatch for $name (file $actual manifest $($parsed[$name]))"
    }
}

Write-Host "Release checksums PASS ($($attached.Count) attached)"
exit 0
