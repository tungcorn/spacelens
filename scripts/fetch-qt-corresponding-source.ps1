# Download and verify the maintainer-pinned Qt 6.8.3 corresponding source.
# Does not unpack the archive. Does not put it in a runtime zip.

[CmdletBinding()]
param(
    [string]$OutDir = "",
    [string]$IdentityFile = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $IdentityFile) {
    $IdentityFile = Join-Path $root "packaging\qt-source\SOURCE_IDENTITY.txt"
}
if (-not $OutDir) {
    $OutDir = Join-Path $root "dist\qt-source"
}

if (-not (Test-Path $IdentityFile)) {
    Write-Error "source identity not found: $IdentityFile"
}

$values = @{}
foreach ($raw in Get-Content $IdentityFile) {
    $line = $raw.Trim()
    if ($line.Length -eq 0 -or $line.StartsWith("#")) { continue }
    $eq = $line.IndexOf("=")
    if ($eq -lt 1) { continue }
    $values[$line.Substring(0, $eq).Trim()] = $line.Substring($eq + 1).Trim()
}

$archive = $values["archive"]
$expected = $values["sha256"]
$url = $values["url"]
$version = $values["version"]
if (-not $archive -or -not $expected -or -not $url) {
    Write-Error "SOURCE_IDENTITY.txt missing archive, sha256, or url"
}
if ($version -and $version -ne "6.8.3") {
    Write-Error "source identity version '$version' is not the packaging pin 6.8.3"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$dest = Join-Path $OutDir $archive

Write-Host "Fetching $url"
Write-Host "Expected SHA-256 $expected"
Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing

$actual = (Get-FileHash -Algorithm SHA256 $dest).Hash.ToLowerInvariant()
if ($actual -ne $expected.ToLowerInvariant()) {
    Remove-Item -Force $dest -ErrorAction SilentlyContinue
    Write-Error "SHA-256 mismatch: got $actual expected $expected"
}

Write-Host "Verified $dest"
Write-Host $actual
exit 0
