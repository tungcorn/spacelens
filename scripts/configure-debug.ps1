$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "dev-env.ps1")

Set-Location $root
cmake --preset windows-debug
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Configured Debug preset windows-debug"
