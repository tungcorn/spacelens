# Update packaging/npm/release-pin.env from a verified public unified zip.
# Never hash a local rebuild or CI staging artifact unless -ZipPath is an
# already-downloaded PUBLIC asset (tests pass fixtures).

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Repo = "tungcorn/spacelens",
    [string]$RepoRoot = "",
    [string]$ZipPath = "",
    [string]$SumsPath = "",
    [switch]$DryRun,
    [switch]$WriteFiles
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

if (-not $RepoRoot) { $RepoRoot = Get-SpaceLensRepoRoot }
$v = ConvertTo-SpaceLensVersion $Version
if (-not $v) { throw "Version '$Version' is not X.Y.Z" }
if ($v.Text -ne $Version.TrimStart('v')) {
    # already normalized
}

$names = Get-SpaceLensArtifactNames $v
$work = $null
$zip = $ZipPath
$sums = $SumsPath

try {
    if (-not $zip -or -not $sums) {
        $work = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-pin-" + [guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Path $work | Out-Null
        if (-not $zip) {
            & gh release download $v.Tag --repo $Repo --pattern $names.Unified --dir $work
            if ($LASTEXITCODE -ne 0) { throw "failed to download $($names.Unified) from $($v.Tag)" }
            $zip = Join-Path $work $names.Unified
        }
        if (-not $sums) {
            & gh release download $v.Tag --repo $Repo --pattern $names.Sums --dir $work
            if ($LASTEXITCODE -ne 0) { throw "failed to download $($names.Sums) from $($v.Tag)" }
            $sums = Join-Path $work $names.Sums
        }
    }

    if (-not (Test-Path -LiteralPath $zip)) { throw "unified zip missing: $zip" }
    if (-not (Test-Path -LiteralPath $sums)) { throw "SHA256SUMS missing: $sums" }

    $observed = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    $sumLine = Get-Content -LiteralPath $sums | Where-Object { $_ -match [regex]::Escape($names.Unified) } | Select-Object -First 1
    if (-not $sumLine) { throw "SHA256SUMS.txt does not list $($names.Unified)" }
    if ($sumLine -notmatch '^([0-9a-fA-F]{64})\s+') {
        throw "unreadable SHA256SUMS line: $sumLine"
    }
    $fromSums = $Matches[1].ToLowerInvariant()
    if ($observed -ne $fromSums) {
        throw "STOP: public zip hash $observed != SHA256SUMS $fromSums"
    }

    $pinPath = Join-Path $RepoRoot "packaging\npm\release-pin.env"
    $current = @{}
    if (Test-Path -LiteralPath $pinPath) {
        foreach ($line in Get-Content -LiteralPath $pinPath) {
            $trim = $line.Trim()
            if (-not $trim -or $trim.StartsWith("#")) { continue }
            $eq = $trim.IndexOf("=")
            if ($eq -lt 1) { continue }
            $current[$trim.Substring(0, $eq).Trim()] = $trim.Substring($eq + 1).Trim()
        }
    }
    $already = ($current.SPACELENS_VERSION -eq $v.Text -and $current.RELEASE_SHA256 -eq $observed)
    $text = New-SpaceLensReleasePinText -Version $v -Sha256 $observed -Repo $Repo

    Write-Host "public_unified=$($names.Unified)"
    Write-Host "public_sha256=$observed"
    Write-Host "pin_already_current=$($already.ToString().ToLowerInvariant())"

    if ($env:GITHUB_OUTPUT) {
        "sha256=$observed" | Out-File $env:GITHUB_OUTPUT -Append
        "already_current=$($already.ToString().ToLowerInvariant())" | Out-File $env:GITHUB_OUTPUT -Append
    }

    if ($already) {
        Write-Host "release pin already matches the public unified hash"
        exit 0
    }
    if ($DryRun -or -not $WriteFiles) {
        Write-Host "pin_would_update=true"
        exit 0
    }

    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($pinPath, $text.TrimEnd() + "`n", $utf8)

    $releasing = Join-Path $RepoRoot "docs\RELEASING.md"
    if (Test-Path -LiteralPath $releasing) {
        $doc = Get-Content -LiteralPath $releasing -Raw
        $doc = [regex]::Replace(
            $doc,
            'currently v\d+\.\d+\.\d+,\r?\n`[0-9a-f]{64}`',
            "currently $($v.Tag),`n``$observed``"
        )
        $doc = [regex]::Replace(
            $doc,
            '`@tungcorn/spacelens@\d+\.\d+\.\d+` is on the public npm registry\.',
            "``@tungcorn/spacelens@$($v.Text)`` is on the public npm registry."
        )
        $doc = [regex]::Replace(
            $doc,
            'Do not retag or republish\r?\n0\.1\.0–0\.\d+\.\d+\.',
            "Do not retag or republish`n0.1.0–$($v.Text)."
        )
        [System.IO.File]::WriteAllText($releasing, $doc, $utf8)
    }

    Write-Host "updated $pinPath"
    exit 0
} finally {
    if ($work -and (Test-Path -LiteralPath $work)) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}
