# Automated test suite for scripts/detect-ci-changes.ps1

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "detect-ci-changes.ps1"
$tab = "`t"

function Run-TestCase(
    [string]$Name,
    [string[]]$StatusLines,
    [string]$ExpectedDocsOnly,
    [string]$ExpectedRunExpensive,
    [string]$ExpectedPinOnly = "false"
) {
    Write-Host "========================================"
    Write-Host "Test: $Name"

    $tmpFile = [System.IO.Path]::GetTempFileName()
    try {
        & $script -ManualFileList $StatusLines -OutputFile $tmpFile

        $outputContent = Get-Content $tmpFile
        $docsOnlyLine = ($outputContent | Where-Object { $_ -like "docs_only=*" }) -replace "docs_only=", ""
        $runExpensiveLine = ($outputContent | Where-Object { $_ -like "run_expensive=*" }) -replace "run_expensive=", ""
        $pinOnlyLine = ($outputContent | Where-Object { $_ -like "pin_only=*" }) -replace "pin_only=", ""

        if ($docsOnlyLine -ne $ExpectedDocsOnly) {
            throw "FAIL: Expected docs_only=$ExpectedDocsOnly, got '$docsOnlyLine'"
        }
        if ($runExpensiveLine -ne $ExpectedRunExpensive) {
            throw "FAIL: Expected run_expensive=$ExpectedRunExpensive, got '$runExpensiveLine'"
        }
        if ($pinOnlyLine -ne $ExpectedPinOnly) {
            throw "FAIL: Expected pin_only=$ExpectedPinOnly, got '$pinOnlyLine'"
        }
        Write-Host "PASS: docs_only=$docsOnlyLine, pin_only=$pinOnlyLine, run_expensive=$runExpensiveLine"
    } finally {
        if (Test-Path $tmpFile) { Remove-Item $tmpFile -Force }
    }
}

# 1. Rename src file to docs file -> Full CI
Run-TestCase "Rename src to docs" @("R100${tab}src/foo.cpp${tab}docs/foo.md") "false" "true"

# 2. Rename doc file to doc file -> Docs-only
Run-TestCase "Rename doc to doc" @("R100${tab}docs/a.md${tab}docs/b.md") "true" "false"

# 3. Deleted src file -> Full CI
Run-TestCase "Delete src file" @("D${tab}src/old.cpp") "false" "true"

# 4. Deleted docs file -> Docs-only
Run-TestCase "Delete doc file" @("D${tab}docs/old.md") "true" "false"

# 5. README-only -> Docs-only
Run-TestCase "README-only modification" @("M${tab}README.md") "true" "false"

# 6. docs/** only -> Docs-only
Run-TestCase "docs/** modification" @("M${tab}docs/INDEX.md", "M${tab}docs/SAFETY.md") "true" "false"

# 7. src/** change -> Full CI
Run-TestCase "src/** modification" @("M${tab}src/core/scanner.cpp") "false" "true"

# 8. Build/script change -> Full CI
Run-TestCase "CMakeLists.txt modification" @("M${tab}CMakeLists.txt") "false" "true"

# 9. Mixed docs + code -> Full CI
Run-TestCase "Mixed docs and code" @("M${tab}README.md", "M${tab}src/core/scanner.cpp") "false" "true"

# 10. Pin-only (hash pin + RELEASING.md) -> cheap pin gate
Run-TestCase "Pin-only with RELEASING.md" @(
    "M${tab}packaging/npm/release-pin.env",
    "M${tab}docs/RELEASING.md"
) "false" "false" "true"

# 11. Pin file alone -> cheap pin gate
Run-TestCase "Pin file only" @("M${tab}packaging/npm/release-pin.env") "false" "false" "true"

# 12. Pin + product code -> Full CI
Run-TestCase "Pin plus product" @(
    "M${tab}packaging/npm/release-pin.env",
    "M${tab}src/cli/main.cpp"
) "false" "true" "false"

Write-Host "`nAll 12 change-detection tests PASSED!"
exit 0
