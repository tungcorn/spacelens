$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "dev-env.ps1")

Set-Location $root
if (-not (Test-Path (Join-Path $root "build-release\build.ninja"))) {
    & (Join-Path $PSScriptRoot "configure-release.ps1")
}
cmake --build --preset windows-release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --preset windows-release
exit $LASTEXITCODE
