# Offline checks for distribution gates. No Qt, no compiler, no network.

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$verifyReview = Join-Path $root "scripts\verify-qt-redist-review.ps1"
$verifySums = Join-Path $root "scripts\verify-release-checksums.ps1"
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-dist-selftest-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $scratch | Out-Null

$failures = New-Object System.Collections.Generic.List[string]
function Add-Fail([string]$Message) {
    $script:failures.Add($Message)
    Write-Host "SELFTEST FAIL: $Message"
}

try {
    function Write-Env([string]$Path, [hashtable]$Values) {
        $lines = @(
            "review_status=$($Values.status)",
            "qt_version=$($Values.qt)",
            "linkage=$($Values.linkage)",
            "source_availability=$($Values.source)"
        )
        $utf8 = New-Object System.Text.UTF8Encoding $false
        [System.IO.File]::WriteAllLines($Path, $lines, $utf8)
    }

    function Invoke-Review([string]$File, [switch]$RequirePass) {
        if ($RequirePass) {
            & $verifyReview -ReviewFile $File -RequirePass
        } else {
            & $verifyReview -ReviewFile $File
        }
        return $LASTEXITCODE
    }

    $pending = Join-Path $scratch "pending.env"
    Write-Env $pending @{ status = "PENDING"; qt = "6.8.3"; linkage = "shared"; source = "MISSING" }
    if ((Invoke-Review $pending) -ne 0) { Add-Fail "well-formed PENDING should exit 0" }
    if ((Invoke-Review $pending -RequirePass) -eq 0) { Add-Fail "PENDING RequirePass should exit 1" }

    $passReady = Join-Path $scratch "pass.env"
    Write-Env $passReady @{ status = "PASS"; qt = "6.8.3"; linkage = "shared"; source = "READY" }
    if ((Invoke-Review $passReady) -ne 0) { Add-Fail "PASS+READY should be well-formed" }
    if ((Invoke-Review $passReady -RequirePass) -ne 0) { Add-Fail "PASS+READY RequirePass should exit 0" }

    $passMissing = Join-Path $scratch "pass-missing.env"
    Write-Env $passMissing @{ status = "PASS"; qt = "6.8.3"; linkage = "shared"; source = "MISSING" }
    if ((Invoke-Review $passMissing) -eq 0) { Add-Fail "PASS+MISSING must be malformed" }

    $passStatic = Join-Path $scratch "pass-static.env"
    Write-Env $passStatic @{ status = "PASS"; qt = "6.8.3"; linkage = "static"; source = "READY" }
    if ((Invoke-Review $passStatic) -eq 0) { Add-Fail "PASS+static must be malformed" }

    $wrongVer = Join-Path $scratch "wrong-ver.env"
    Write-Env $wrongVer @{ status = "PASS"; qt = "6.7.0"; linkage = "shared"; source = "READY" }
    if ((Invoke-Review $wrongVer) -eq 0) { Add-Fail "wrong qt_version must fail" }
    if ((Invoke-Review $wrongVer -RequirePass) -eq 0) { Add-Fail "wrong qt_version RequirePass must fail" }

    $emptySentinel = Join-Path $scratch "QT_REDIST_REVIEWED.md"
    Set-Content -Path $emptySentinel -Value "" -NoNewline
    if ((Invoke-Review $pending -RequirePass) -eq 0) {
        Add-Fail "empty QT_REDIST_REVIEWED.md must not unlock RequirePass"
    }

    $repoLicense = Join-Path $root "LICENSE"
    if (-not (Test-Path $repoLicense)) {
        Add-Fail "root LICENSE is missing"
    } else {
        $licenseText = Get-Content $repoLicense -Raw
        if ([string]::IsNullOrWhiteSpace($licenseText)) {
            Add-Fail "root LICENSE is empty"
        }
        if ($licenseText -notmatch "Permission is hereby granted") {
            Add-Fail "root LICENSE does not look like MIT"
        }
        if ($licenseText -notmatch "Copyright \(c\) 2026 tungcorn") {
            Add-Fail "root LICENSE copyright notice is not the recorded holder"
        }
    }

    $cliZip = Join-Path $scratch "spacelens-cli-v0.1.1-windows-x64.zip"
    $mainZip = Join-Path $scratch "spacelens-v0.1.1-windows-x64.zip"
    Set-Content -Path $cliZip -Value "cli-bytes" -NoNewline
    Set-Content -Path $mainZip -Value "main-bytes" -NoNewline
    $cliHash = (Get-FileHash -Algorithm SHA256 $cliZip).Hash.ToLowerInvariant()
    $mainHash = (Get-FileHash -Algorithm SHA256 $mainZip).Hash.ToLowerInvariant()
    $fullSums = Join-Path $scratch "SHA256SUMS-full.txt"
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllLines($fullSums, @(
        "$cliHash  spacelens-cli-v0.1.1-windows-x64.zip",
        "$mainHash  spacelens-v0.1.1-windows-x64.zip"
    ), $utf8)

    & $verifySums -SumsPath $fullSums -ZipDir $scratch -AttachedNames @(
        "spacelens-cli-v0.1.1-windows-x64.zip",
        "spacelens-v0.1.1-windows-x64.zip"
    )
    if ($LASTEXITCODE -ne 0) { Add-Fail "CLI+unified checksum set should PASS" }

    $cliOnlySums = Join-Path $scratch "SHA256SUMS-cli.txt"
    [System.IO.File]::WriteAllLines($cliOnlySums, @(
        "$cliHash  spacelens-cli-v0.1.1-windows-x64.zip"
    ), $utf8)
    & $verifySums -SumsPath $cliOnlySums -ZipDir $scratch -AttachedNames @(
        "spacelens-cli-v0.1.1-windows-x64.zip"
    )
    if ($LASTEXITCODE -ne 0) { Add-Fail "CLI-only filtered checksums should PASS" }

    $cliOnlyAgainstFull = 0
    try {
        & $verifySums -SumsPath $fullSums -ZipDir $scratch -AttachedNames @(
            "spacelens-cli-v0.1.1-windows-x64.zip"
        )
        $cliOnlyAgainstFull = $LASTEXITCODE
    } catch {
        $cliOnlyAgainstFull = 1
    }
    if ($cliOnlyAgainstFull -eq 0) {
        Add-Fail "full checksums must not be accepted as a CLI-only publish set"
    }

    $identity = Join-Path $root "packaging\qt-source\SOURCE_IDENTITY.txt"
    if (-not (Test-Path $identity)) {
        Add-Fail "SOURCE_IDENTITY.txt missing"
    } else {
        $idText = Get-Content $identity -Raw
        if ($idText -notmatch "cdd3a69967208276bb01af7ace7dba0ba53e679f886a4cbe624225c60fb73f2c") {
            Add-Fail "SOURCE_IDENTITY.txt missing official Qt 6.8.3 SHA-256"
        }
        if ($idText -notmatch "qt-everywhere-src-6.8.3.tar.xz") {
            Add-Fail "SOURCE_IDENTITY.txt missing official archive name"
        }
    }
} finally {
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host "Distribution selftest failed ($($failures.Count) problem(s))"
    exit 1
}

Write-Host "Distribution selftest PASS"
exit 0
