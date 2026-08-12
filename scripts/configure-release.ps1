$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "dev-env.ps1")

$buildDir = Join-Path $root "build"
cmake -S $root -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$env:CMAKE_PREFIX_PATH"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Configured Release in $buildDir"
