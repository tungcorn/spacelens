$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "dev-env.ps1")

$buildDir = Join-Path $root "build"
if (-not (Test-Path (Join-Path $buildDir "build.ninja"))) {
    & (Join-Path $PSScriptRoot "configure-release.ps1")
}
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --test-dir $buildDir --output-on-failure
exit $LASTEXITCODE
