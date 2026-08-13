# Parse packaging/qt-redist-review.env.
# Well-formed PENDING records are allowed (exit 0) so ordinary CI can
# package verification zips. GUI publish eligibility requires -RequirePass,
# which succeeds only for review_status=PASS + matching qt_version +
# linkage=shared + source_availability=READY.
# Presence of docs/QT_REDIST_REVIEWED.md is ignored.

[CmdletBinding()]
param(
    [string]$ReviewFile = "",
    [string]$ExpectedQtVersion = "6.8.3",
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $ReviewFile) {
    $ReviewFile = Join-Path $root "packaging\qt-redist-review.env"
}

function Write-ReviewFail([string]$Message) {
    # exit 1 (do not Write-Error): callers probe -RequirePass via LASTEXITCODE
    # without aborting the package job.
    Write-Host "QT_REDIST_REVIEW: $Message"
    exit 1
}

if (-not (Test-Path $ReviewFile)) {
    Write-ReviewFail "review file not found: $ReviewFile"
}

$allowedStatus = @("PENDING", "PASS", "FAIL")
$allowedLinkage = @("shared", "static")
$allowedSource = @("MISSING", "READY")
$requiredKeys = @("review_status", "qt_version", "linkage", "source_availability")

$values = @{}
$lineNo = 0
foreach ($raw in Get-Content $ReviewFile) {
    $lineNo += 1
    $line = $raw.Trim()
    if ($line.Length -eq 0 -or $line.StartsWith("#")) { continue }
    $eq = $line.IndexOf("=")
    if ($eq -lt 1) {
        Write-ReviewFail "line ${lineNo}: expected KEY=VALUE"
    }
    $key = $line.Substring(0, $eq).Trim()
    $val = $line.Substring($eq + 1).Trim()
    if ($values.ContainsKey($key)) {
        Write-ReviewFail "duplicate key '$key'"
    }
    $values[$key] = $val
}

foreach ($key in $requiredKeys) {
    if (-not $values.ContainsKey($key) -or [string]::IsNullOrWhiteSpace($values[$key])) {
        Write-ReviewFail "missing required key '$key'"
    }
}

foreach ($key in $values.Keys) {
    if ($requiredKeys -notcontains $key) {
        Write-ReviewFail "unknown key '$key' (allowed: $($requiredKeys -join ', '))"
    }
}

$status = $values["review_status"]
$qtVersion = $values["qt_version"]
$linkage = $values["linkage"]
$source = $values["source_availability"]

if ($allowedStatus -notcontains $status) {
    Write-ReviewFail "review_status='$status' (allowed: $($allowedStatus -join ', '))"
}
if ($allowedLinkage -notcontains $linkage) {
    Write-ReviewFail "linkage='$linkage' (allowed: $($allowedLinkage -join ', '))"
}
if ($allowedSource -notcontains $source) {
    Write-ReviewFail "source_availability='$source' (allowed: $($allowedSource -join ', '))"
}
if ($qtVersion -ne $ExpectedQtVersion) {
    Write-ReviewFail "qt_version='$qtVersion' does not match packaging pin '$ExpectedQtVersion'"
}

# Contradictory combinations are never well-formed.
if ($status -eq "PASS" -and $source -ne "READY") {
    Write-ReviewFail "review_status=PASS requires source_availability=READY (got $source)"
}
if ($status -eq "PASS" -and $linkage -ne "shared") {
    Write-ReviewFail "review_status=PASS requires linkage=shared (this product ships the shared kit)"
}

Write-Host "qt-redist-review: status=$status qt_version=$qtVersion linkage=$linkage source_availability=$source"

if ($RequirePass) {
    if ($status -ne "PASS") {
        Write-ReviewFail "RequirePass: review_status is $status, not PASS"
    }
    if ($source -ne "READY") {
        Write-ReviewFail "RequirePass: source_availability is $source, not READY"
    }
    if ($linkage -ne "shared") {
        Write-ReviewFail "RequirePass: linkage is $linkage, not shared"
    }
    if ($qtVersion -ne $ExpectedQtVersion) {
        Write-ReviewFail "RequirePass: qt_version mismatch"
    }
    Write-Host "qt-redist-review: PASS (structured)"
}

exit 0
