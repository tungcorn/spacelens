# Release Automation V2 preflight.
# Refuses publish when the requested version disagrees with CMake, the tag
# already exists, npm already has that version, or required CI is not green.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [switch]$Publish,
    [string]$Repository = "",
    [string]$Sha = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    Write-Error "version '$Version' is not X.Y.Z"
}

$cmakeText = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmakeText -notmatch 'project\(\s*SpaceLens\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    Write-Error "CMake project VERSION not found"
}
$cmakeVersion = $Matches[1]
if ($cmakeVersion -ne $Version) {
    Write-Error "requested version $Version does not match CMake $cmakeVersion"
}

$pkg = Get-Content (Join-Path $root "packaging\npm\package.json") -Raw | ConvertFrom-Json
if ($pkg.version -ne $Version) {
    Write-Error "package.json version $($pkg.version) does not match $Version"
}

$tag = "v$Version"
Write-Host "preflight version=$Version tag=$tag publish=$Publish"

if (-not $Repository) {
    $Repository = $env:GITHUB_REPOSITORY
}
if (-not $Sha) {
    $Sha = $env:GITHUB_SHA
}

if ($Publish) {
    if ($env:GITHUB_REF -and $env:GITHUB_REF -ne "refs/heads/main") {
        Write-Error "publish is allowed only from main (ref=$($env:GITHUB_REF))"
    }

    if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
        $PSNativeCommandUseErrorActionPreference = $false
    }

    $peeled = (
        git ls-remote --tags origin "refs/tags/${tag}^{}" |
            ForEach-Object { ($_ -split '\s+')[0] } |
            Select-Object -First 1
    )
    if (-not $peeled) {
        $peeled = (
            git ls-remote --tags origin "refs/tags/$tag" |
                ForEach-Object { ($_ -split '\s+')[0] } |
                Select-Object -First 1
        )
    }
    if ($peeled -and $Sha -and $peeled -ne $Sha) {
        Write-Error "tag $tag already exists on origin at $peeled; refusing to move it"
    }
    if ($peeled -and $Sha -and $peeled -eq $Sha) {
        Write-Host "origin tag $tag already points at $Sha; Release-create retry allowed if Release is missing"
    }

    $local = git rev-parse -q --verify "${tag}^{commit}" 2>$null
    if ($LASTEXITCODE -eq 0 -and $local -and $Sha -and $local.Trim() -ne $Sha) {
        Write-Error "local tag $tag points at $($local.Trim()), not $Sha"
    }

    gh release view $tag --repo $Repository 1>$null 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Error "GitHub Release $tag already exists"
    }

    $npmVer = npm view "@tungcorn/spacelens@$Version" version 2>$null
    if ($npmVer -eq $Version) {
        Write-Error "npm already has @tungcorn/spacelens@$Version"
    }

    if (-not $Sha) {
        Write-Error "commit SHA required to verify CI"
    }
    $required = @(
        "Windows / Full Debug",
        "Windows / Full Release",
        "Windows / CLI-only Latest",
        "Quality / MSVC Analyze",
        "npm / Package"
    )
    $json = gh api --paginate "repos/$Repository/commits/$Sha/check-runs"
    $runs = $json | ConvertFrom-Json
    $checks = @($runs.check_runs)
    foreach ($name in $required) {
        $mine = @(
            $checks |
                Where-Object { $_.name -eq $name } |
                Sort-Object completed_at -Descending
        )
        if ($mine.Count -eq 0) {
            Write-Error "required CI check missing on ${Sha}: $name"
        }
        $latest = $mine[0]
        if ($latest.conclusion -ne "success") {
            Write-Error "required CI check '$name' is $($latest.status)/$($latest.conclusion)"
        }
        Write-Host "CI OK: $name"
    }
}

Write-Host "release preflight PASS"
exit 0
