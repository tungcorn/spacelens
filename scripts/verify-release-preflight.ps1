# Release publication preflight.
# Refuses publish when the requested version disagrees with CMake, the tag
# points at another SHA, or required CI is not green. Existing correct
# GitHub Release / npm version are verified, not treated as errors.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [switch]$Publish,
    [switch]$Wait,
    [int]$WaitMinutes = 120,
    [string]$Repository = "",
    [string]$Sha = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    Write-Error "version '$Version' is not X.Y.Z"
}

$cmakeVersion = (Get-SpaceLensCMakeVersion -RepoRoot $root).Text
if ($cmakeVersion -ne $Version) {
    Write-Error "requested version $Version does not match CMake $cmakeVersion"
}

$pkgVersion = (Get-SpaceLensNpmVersion -RepoRoot $root).Text
if ($pkgVersion -ne $Version) {
    Write-Error "package.json version $pkgVersion does not match $Version"
}

$tag = "v$Version"
Write-Host "preflight version=$Version tag=$tag publish=$Publish wait=$Wait"

if (-not $Repository) {
    $Repository = $env:GITHUB_REPOSITORY
}
if (-not $Sha) {
    $Sha = $env:GITHUB_SHA
}

function Get-RequiredCheckRuns {
    param([string]$Repo, [string]$Commit)
    $json = gh api --paginate "repos/$Repo/commits/$Commit/check-runs"
    $runs = $json | ConvertFrom-Json
    return @($runs.check_runs)
}

if ($Publish) {
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

    if ($env:GITHUB_REF -and $env:GITHUB_REF -ne "refs/heads/main") {
        if ($peeled -and $Sha -and $peeled -eq $Sha) {
            Write-Host "non-main recovery allowed; tag $tag already points at $Sha (ref=$($env:GITHUB_REF))"
        } else {
            Write-Error "publish is allowed only from main unless $tag already points at this SHA (ref=$($env:GITHUB_REF))"
        }
    }

    $local = git rev-parse -q --verify "${tag}^{commit}" 2>$null
    if ($LASTEXITCODE -eq 0 -and $local -and $Sha -and $local.Trim() -ne $Sha) {
        Write-Error "local tag $tag points at $($local.Trim()), not $Sha"
    }

    $names = Get-SpaceLensArtifactNames $Version
    gh release view $tag --repo $Repository --json tagName,isDraft,isPrerelease,assets 1>$null 2>$null
    if ($LASTEXITCODE -eq 0) {
        $info = gh release view $tag --repo $Repository --json tagName,isDraft,isPrerelease,assets | ConvertFrom-Json
        if ($info.isDraft) { Write-Error "GitHub Release $tag exists but is a draft" }
        if ($info.isPrerelease) { Write-Error "GitHub Release $tag exists but is a prerelease" }
        $assetNames = @($info.assets | ForEach-Object { $_.name })
        foreach ($need in @($names.Unified, $names.Headless, $names.Sums)) {
            if ($assetNames -notcontains $need) {
                Write-Error "GitHub Release $tag exists but is missing $need"
            }
        }
        Write-Host "GitHub Release $tag already exists with required assets; treating as verified"
    } else {
        Write-Host "GitHub Release $tag does not exist yet"
    }

    $npmVer = npm view "@tungcorn/spacelens@$Version" version 2>$null
    if ($npmVer -eq $Version) {
        Write-Host "npm already has @tungcorn/spacelens@$Version; treating as verified"
    } else {
        Write-Host "npm does not have @tungcorn/spacelens@$Version yet"
    }

    if (-not $Sha) {
        Write-Error "commit SHA required to verify CI"
    }
    $required = @(
        "Windows / Full Debug",
        "Windows / Full Release",
        "Windows / CLI-only Latest",
        "Quality / MSVC Analyze",
        "npm / Package",
        "Release / Policy"
    )

    $deadline = [datetime]::UtcNow.AddMinutes($WaitMinutes)
    while ($true) {
        $checks = Get-RequiredCheckRuns -Repo $Repository -Commit $Sha
        $pending = New-Object System.Collections.Generic.List[string]
        $failed = New-Object System.Collections.Generic.List[string]
        $missing = New-Object System.Collections.Generic.List[string]
        foreach ($name in $required) {
            $mine = @(
                $checks |
                    Where-Object { $_.name -eq $name } |
                    Sort-Object completed_at -Descending
            )
            if ($mine.Count -eq 0) {
                $missing.Add($name)
                continue
            }
            $latestCheck = $mine[0]
            if ($latestCheck.conclusion -eq "success") {
                Write-Host "CI OK: $name"
                continue
            }
            if ($latestCheck.status -ne "completed") {
                $pending.Add("$name ($($latestCheck.status))")
                continue
            }
            $failed.Add("$name $($latestCheck.status)/$($latestCheck.conclusion)")
        }

        if ($failed.Count -gt 0) {
            Write-Error "required CI check failed: $($failed -join ', ')"
        }
        if ($missing.Count -eq 0 -and $pending.Count -eq 0) {
            break
        }
        if (-not $Wait) {
            if ($missing.Count -gt 0) {
                Write-Error "required CI check missing on ${Sha}: $($missing -join ', ')"
            }
            if ($pending.Count -gt 0) {
                Write-Error "required CI check not finished: $($pending -join ', ')"
            }
        }
        if ([datetime]::UtcNow -ge $deadline) {
            Write-Error "timed out waiting for CI ($($missing + $pending -join ', '))"
        }
        Write-Host "waiting for CI: $((@($missing) + @($pending)) -join ', ')"
        Start-Sleep -Seconds 30
    }
}

Write-Host "release preflight PASS"
exit 0
