$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "dev-env.ps1")

Set-Location $root
cmake --preset windows-release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Configured Release preset windows-release"
