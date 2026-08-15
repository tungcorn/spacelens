# SpaceLens Release Automation V1 policy library.
# Dot-source from other scripts. Do not execute this file directly.

Set-StrictMode -Version Latest

function Get-SpaceLensRepoRoot {
    if ($PSScriptRoot) {
        return (Split-Path -Parent $PSScriptRoot)
    }
    return (Get-Location).Path
}

function ConvertTo-SpaceLensVersion {
    param([Parameter(Mandatory = $true)][string]$Text)
    $trim = $Text.Trim()
    if ($trim -match '^v?(\d+)\.(\d+)\.(\d+)$') {
        return [pscustomobject]@{
            Major = [int]$Matches[1]
            Minor = [int]$Matches[2]
            Patch = [int]$Matches[3]
            Text  = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
            Tag   = "v$($Matches[1]).$($Matches[2]).$($Matches[3])"
        }
    }
    return $null
}

function Compare-SpaceLensVersion {
    param($Left, $Right)
    $a = if ($Left -is [string]) { ConvertTo-SpaceLensVersion $Left } else { $Left }
    $b = if ($Right -is [string]) { ConvertTo-SpaceLensVersion $Right } else { $Right }
    if (-not $a -or -not $b) {
        throw "cannot compare versions '$Left' and '$Right'"
    }
    if ($a.Major -ne $b.Major) { return [Math]::Sign($a.Major - $b.Major) }
    if ($a.Minor -ne $b.Minor) { return [Math]::Sign($a.Minor - $b.Minor) }
    return [Math]::Sign($a.Patch - $b.Patch)
}

function Get-SpaceLensNextPatchVersion {
    param([Parameter(Mandatory = $true)]$Version)
    $v = if ($Version -is [string]) { ConvertTo-SpaceLensVersion $Version } else { $Version }
    if (-not $v) { throw "not a patchable version: $Version" }
    return [pscustomobject]@{
        Major = $v.Major
        Minor = $v.Minor
        Patch = $v.Patch + 1
        Text  = "$($v.Major).$($v.Minor).$($v.Patch + 1)"
        Tag   = "v$($v.Major).$($v.Minor).$($v.Patch + 1)"
    }
}

function Get-SpaceLensCMakeVersionFromText {
    param([Parameter(Mandatory = $true)][string]$Text)
    if ($Text -notmatch 'project\(\s*SpaceLens\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        throw "CMake project VERSION not found"
    }
    $v = ConvertTo-SpaceLensVersion $Matches[1]
    if (-not $v) { throw "CMake version is not X.Y.Z" }
    return $v
}

function Get-SpaceLensCMakeVersion {
    param([string]$RepoRoot = (Get-SpaceLensRepoRoot))
    return (Get-SpaceLensCMakeVersionFromText (Get-Content (Join-Path $RepoRoot "CMakeLists.txt") -Raw))
}

function Get-SpaceLensTagPeelSha {
    param(
        [Parameter(Mandatory = $true)][string]$Tag,
        [string]$RepoRoot = (Get-SpaceLensRepoRoot)
    )
    $sha = & git -C $RepoRoot rev-parse -q --verify "${Tag}^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0 -or -not "$sha".Trim()) {
        return $null
    }
    return $sha.Trim()
}

function Test-SpaceLensAutoPublishCommit {
    param(
        [Parameter(Mandatory = $true)]$CMakeVersion,
        [Parameter(Mandatory = $true)]$LatestStableVersion,
        $ParentCMakeVersion = $null,
        [string]$HeadSha = "",
        [string]$TagPeelSha = ""
    )
    $cmake = if ($CMakeVersion -is [string]) { ConvertTo-SpaceLensVersion $CMakeVersion } else { $CMakeVersion }
    $latest = if ($LatestStableVersion -is [string]) { ConvertTo-SpaceLensVersion $LatestStableVersion } else { $LatestStableVersion }
    if (-not $cmake -or -not $latest) { return $false }
    $next = Get-SpaceLensNextPatchVersion $latest
    if ($cmake.Text -ne $next.Text) { return $false }

    $parent = $null
    if ($ParentCMakeVersion) {
        $parent = if ($ParentCMakeVersion -is [string]) { ConvertTo-SpaceLensVersion $ParentCMakeVersion } else { $ParentCMakeVersion }
    }
    $isBump = $parent -and ($parent.Text -ne $cmake.Text)
    $tagHere = [bool]($TagPeelSha -and $HeadSha -and ($TagPeelSha -eq $HeadSha))
    return [bool]($isBump -or $tagHere)
}

function Get-SpaceLensNpmVersion {
    param([string]$RepoRoot = (Get-SpaceLensRepoRoot))
    $pkg = Get-Content (Join-Path $RepoRoot "packaging\npm\package.json") -Raw | ConvertFrom-Json
    $v = ConvertTo-SpaceLensVersion ([string]$pkg.version)
    if (-not $v) { throw "package.json version is not X.Y.Z" }
    return $v
}

function Get-SpaceLensLatestStableVersion {
    param([string[]]$Tags)
    $best = $null
    foreach ($tag in $Tags) {
        $v = ConvertTo-SpaceLensVersion $tag
        if (-not $v) { continue }
        if (-not $best -or (Compare-SpaceLensVersion $v $best) -gt 0) {
            $best = $v
        }
    }
    return $best
}

function Get-SpaceLensGitTags {
    param([string]$RepoRoot = (Get-SpaceLensRepoRoot))
    $raw = & git -C $RepoRoot tag --list "v*"
    if ($LASTEXITCODE -ne 0) { throw "git tag --list failed" }
    return @($raw | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

function Test-SpaceLensReleaseLoopSubject {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Subject)
    $s = $Subject.Trim()
    if ($s -match '^(?:release|chore\(release\)):\s*prepare SpaceLens v\d+\.\d+\.\d+\s*$') {
        return $true
    }
    if ($s -match '^chore\(npm\):\s*pin pack-from-release to public v\d+\.\d+\.\d+ hash\s*$') {
        return $true
    }
    return $false
}

function Test-SpaceLensReleasePrepSubject {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Subject,
        [string]$ExpectedVersion = ""
    )
    if ($Subject -notmatch '^(?:release|chore\(release\)):\s*prepare SpaceLens v(\d+\.\d+\.\d+)\s*$') {
        return $false
    }
    if ($ExpectedVersion -and $Matches[1] -ne $ExpectedVersion) {
        return $false
    }
    return $true
}

function Get-SpaceLensCommitKind {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Subject)
    $s = $Subject.Trim()
    if ($s -match '^(feat|fix|perf|refactor|docs|test|ci|chore|style|build)(?:\([^)]+\))?!?:') {
        return $Matches[1]
    }
    if ($s -match '^Merge ') { return "merge" }
    return "other"
}

function Test-SpaceLensProductPath {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path)
    $norm = ($Path -replace '\\', '/').TrimStart('./')
    $prefixes = @(
        'src/core/',
        'src/cli/',
        'src/mcp/',
        'src/app/',
        'src/ui/',
        'src/platform/'
    )
    foreach ($prefix in $prefixes) {
        if ($norm.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Test-SpaceLensReleasableCommit {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Subject,
        [string[]]$Paths = @()
    )
    if (Test-SpaceLensReleaseLoopSubject $Subject) {
        return $false
    }
    $kind = Get-SpaceLensCommitKind $Subject
    if ($kind -in @('feat', 'fix', 'perf')) {
        return $true
    }
    $touchesProduct = $false
    foreach ($path in $Paths) {
        if (Test-SpaceLensProductPath $path) {
            $touchesProduct = $true
            break
        }
    }
    if ($touchesProduct -and $kind -in @('docs', 'test', 'ci', 'chore', 'style', 'build', 'refactor', 'other', 'merge')) {
        return $true
    }
    return $false
}

function Get-SpaceLensReleaseDecision {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Commits,
        [Parameter(Mandatory = $true)]$LatestVersion
    )
    $latest = if ($LatestVersion -is [string]) { ConvertTo-SpaceLensVersion $LatestVersion } else { $LatestVersion }
    if (-not $latest) { throw "latest version is required" }

    $releasable = New-Object System.Collections.Generic.List[object]
    foreach ($commit in $Commits) {
        $subject = [string]$commit.Subject
        $paths = @()
        if ($commit.PSObject.Properties.Name -contains 'Paths' -and $commit.Paths) {
            $paths = @($commit.Paths)
        }
        if (Test-SpaceLensReleasableCommit -Subject $subject -Paths $paths) {
            $releasable.Add($commit)
        }
    }

    $needed = $releasable.Count -gt 0
    $next = $null
    if ($needed) {
        $next = Get-SpaceLensNextPatchVersion $latest
    }
    return [pscustomobject]@{
        Needed          = $needed
        LatestVersion   = $latest
        NextVersion     = $next
        ReleasableCount = $releasable.Count
        Releasable      = $releasable.ToArray()
    }
}

function Get-SpaceLensCommitsSinceTag {
    param(
        [Parameter(Mandatory = $true)][string]$Tag,
        [string]$Head = "HEAD",
        [string]$RepoRoot = (Get-SpaceLensRepoRoot)
    )
    $range = "$Tag..$Head"
    $raw = & git -C $RepoRoot log $range --format="__COMMIT__%n%H%n%s" --name-only
    if ($LASTEXITCODE -ne 0) {
        throw "git log $range failed"
    }
    $commits = New-Object System.Collections.Generic.List[object]
    $hash = $null
    $subject = $null
    $paths = New-Object System.Collections.Generic.List[string]
    foreach ($line in @($raw)) {
        if ($line -eq "__COMMIT__") {
            if ($hash) {
                $commits.Add([pscustomobject]@{
                    Hash    = $hash
                    Subject = $subject
                    Paths   = $paths.ToArray()
                })
            }
            $hash = $null
            $subject = $null
            $paths = New-Object System.Collections.Generic.List[string]
            continue
        }
        if (-not $hash) {
            $hash = $line.Trim()
            continue
        }
        if ($null -eq $subject) {
            $subject = $line
            continue
        }
        if ($line.Trim()) {
            $paths.Add($line.Trim())
        }
    }
    if ($hash) {
        $commits.Add([pscustomobject]@{
            Hash    = $hash
            Subject = $subject
            Paths   = $paths.ToArray()
        })
    }
    return $commits.ToArray()
}

function Get-SpaceLensArtifactNames {
    param([Parameter(Mandatory = $true)]$Version)
    $v = if ($Version -is [string]) { ConvertTo-SpaceLensVersion $Version } else { $Version }
    if (-not $v) { throw "invalid version for artifacts" }
    return [pscustomobject]@{
        Unified  = "spacelens-$($v.Tag)-windows-x64.zip"
        Headless = "spacelens-cli-$($v.Tag)-windows-x64.zip"
        Sums     = "SHA256SUMS.txt"
        Tag      = $v.Tag
        Version  = $v.Text
    }
}

function New-SpaceLensReleasePinText {
    param(
        [Parameter(Mandatory = $true)]$Version,
        [Parameter(Mandatory = $true)][string]$Sha256,
        [string]$Repo = "tungcorn/spacelens"
    )
    $v = if ($Version -is [string]) { ConvertTo-SpaceLensVersion $Version } else { $Version }
    if (-not $v) { throw "invalid pin version" }
    $sha = $Sha256.Trim().ToLowerInvariant()
    if ($sha -notmatch '^[0-9a-f]{64}$') {
        throw "pin sha256 is not a 64-char hex digest"
    }
    $asset = "spacelens-$($v.Tag)-windows-x64.zip"
    $url = "https://github.com/$Repo/releases/download/$($v.Tag)/$asset"
    return @"
# Immutable pin for the published SpaceLens $($v.Tag) unified archive.
# package-npm.ps1 refuses to stage any other hash. Do not "fix" a mismatch
# by changing this file — STOP and investigate the public release asset.

SPACELENS_VERSION=$($v.Text)
NPM_PACKAGE_NAME=@tungcorn/spacelens
RELEASE_TAG=$($v.Tag)
RELEASE_ASSET=$asset
RELEASE_SHA256=$sha
RELEASE_URL=$url
RELEASE_REPO=$Repo
"@
}

function Get-SpaceLensPublishPlan {
    param(
        [Parameter(Mandatory = $true)]$PreparedVersion,
        [Parameter(Mandatory = $true)]$LatestStableVersion,
        [string]$TagPeelSha = "",
        [string]$HeadSha = "",
        [bool]$GitHubReleaseExists = $false,
        [bool]$NpmVersionExists = $false,
        [string]$CurrentPinVersion = "",
        [string]$CurrentPinSha256 = "",
        [string]$PublicUnifiedSha256 = ""
    )
    $prepared = if ($PreparedVersion -is [string]) { ConvertTo-SpaceLensVersion $PreparedVersion } else { $PreparedVersion }
    $latest = if ($LatestStableVersion -is [string]) { ConvertTo-SpaceLensVersion $LatestStableVersion } else { $LatestStableVersion }
    $next = Get-SpaceLensNextPatchVersion $latest
    $isNextPatch = $prepared.Text -eq $next.Text
    $tagPointsHere = $TagPeelSha -and $HeadSha -and ($TagPeelSha -eq $HeadSha)
    $tagPointsElsewhere = $TagPeelSha -and $HeadSha -and ($TagPeelSha -ne $HeadSha)

    $createTag = $false
    $createRelease = $false
    $publishNpm = $false
    $refuse = $null

    if ($tagPointsElsewhere) {
        $refuse = "tag $($prepared.Tag) already points at $TagPeelSha, not $HeadSha"
    } elseif (-not $isNextPatch -and -not $tagPointsHere) {
        $refuse = $null
    } else {
        $createTag = -not $TagPeelSha
        $createRelease = -not $GitHubReleaseExists
        $publishNpm = -not $NpmVersionExists
    }

    $updatePin = $false
    if ($PublicUnifiedSha256) {
        $pub = $PublicUnifiedSha256.Trim().ToLowerInvariant()
        $pinVerMatch = $CurrentPinVersion -eq $prepared.Text
        $pinHashMatch = $CurrentPinSha256 -and ($CurrentPinSha256.Trim().ToLowerInvariant() -eq $pub)
        $updatePin = -not ($pinVerMatch -and $pinHashMatch)
    }

    $alreadyComplete = $tagPointsHere -and $GitHubReleaseExists -and $NpmVersionExists -and -not $updatePin

    return [pscustomobject]@{
        PreparedVersion = $prepared
        IsNextPatch     = $isNextPatch
        CreateTag       = $createTag
        CreateRelease   = $createRelease
        PublishNpm      = $publishNpm
        UpdatePin       = $updatePin
        AlreadyComplete = [bool]$alreadyComplete
        Refuse          = $refuse
        ShouldPublish   = [bool]($isNextPatch -or $tagPointsHere) -and -not $refuse
    }
}

function Get-SpaceLensMechanicalVersionFiles {
    return @(
        "CMakeLists.txt",
        "packaging/npm/package.json",
        "src/cli/main.cpp",
        "src/cli/Commands.cpp",
        "src/mcp/Protocol.hpp",
        "src/app/Application.cpp",
        "src/core/StorageAnalysis.cpp",
        ".github/workflows/release.yml",
        ".github/workflows/npm-publish.yml"
    )
}

function Update-SpaceLensMechanicalVersions {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$FromVersion,
        [Parameter(Mandatory = $true)][string]$ToVersion
    )
    $from = [regex]::Escape($FromVersion)
    $replacements = @(
        @{ Rel = "CMakeLists.txt"; Pattern = "(project\(\s*SpaceLens\s+VERSION\s+)$from"; Replace = "`${1}$ToVersion" },
        @{ Rel = "packaging/npm/package.json"; Pattern = '("version"\s*:\s*")' + $from + '(")'; Replace = "`${1}$ToVersion`${2}" },
        @{ Rel = "src/cli/main.cpp"; Pattern = '(#define SPACELENS_VERSION_STRING ")' + $from + '(")'; Replace = "`${1}$ToVersion`${2}" },
        @{ Rel = "src/cli/Commands.cpp"; Pattern = '(#define SPACELENS_VERSION_STRING ")' + $from + '(")'; Replace = "`${1}$ToVersion`${2}" },
        @{ Rel = "src/app/Application.cpp"; Pattern = '(#define SPACELENS_VERSION_STRING ")' + $from + '(")'; Replace = "`${1}$ToVersion`${2}" },
        @{ Rel = "src/core/StorageAnalysis.cpp"; Pattern = '(#define SPACELENS_VERSION_STRING ")' + $from + '(")'; Replace = "`${1}$ToVersion`${2}" },
        @{ Rel = "src/mcp/Protocol.hpp"; Pattern = '(#define SPACELENS_MCP_VERSION ")' + $from + '(")'; Replace = "`${1}$ToVersion`${2}" },
        @{ Rel = ".github/workflows/release.yml"; Pattern = '(default:\s*")' + $from + '(")'; Replace = "`${1}$ToVersion`${2}" },
        @{ Rel = ".github/workflows/npm-publish.yml"; Pattern = '(default:\s*")' + $from + '(")'; Replace = "`${1}$ToVersion`${2}" }
    )
    $changed = New-Object System.Collections.Generic.List[string]
    foreach ($item in $replacements) {
        $path = Join-Path $RepoRoot ($item.Rel -replace '/', [IO.Path]::DirectorySeparatorChar)
        if (-not (Test-Path -LiteralPath $path)) {
            throw "version file missing: $($item.Rel)"
        }
        $text = Get-Content -LiteralPath $path -Raw
        $updated = [regex]::Replace($text, $item.Pattern, $item.Replace, 1)
        if ($updated -eq $text) {
            throw "did not update version in $($item.Rel)"
        }
        $utf8 = New-Object System.Text.UTF8Encoding $false
        [System.IO.File]::WriteAllText($path, $updated, $utf8)
        $changed.Add($item.Rel)
    }
    return $changed.ToArray()
}

function Format-SpaceLensGeneratedNotes {
    param(
        [Parameter(Mandatory = $true)]$Version,
        [Parameter(Mandatory = $true)]$PreviousVersion,
        [object[]]$Commits = @()
    )
    $v = if ($Version -is [string]) { ConvertTo-SpaceLensVersion $Version } else { $Version }
    $prev = if ($PreviousVersion -is [string]) { ConvertTo-SpaceLensVersion $PreviousVersion } else { $PreviousVersion }
    $added = New-Object System.Collections.Generic.List[string]
    $fixed = New-Object System.Collections.Generic.List[string]
    $perf = New-Object System.Collections.Generic.List[string]
    $other = New-Object System.Collections.Generic.List[string]
    foreach ($commit in $Commits) {
        $subject = [string]$commit.Subject
        if (Test-SpaceLensReleaseLoopSubject $subject) { continue }
        $kind = Get-SpaceLensCommitKind $subject
        $rest = $subject
        if ($subject -match '^[a-z]+(?:\(([^)]+)\))?!?:\s*(.+)$') {
            $scope = $Matches[1]
            $summary = $Matches[2]
            if ($scope) { $rest = "${scope}: $summary" } else { $rest = $summary }
        }
        $line = "- $rest"
        switch ($kind) {
            "feat" { $added.Add($line) }
            "fix" { $fixed.Add($line) }
            "perf" { $perf.Add($line) }
            default {
                if (Test-SpaceLensReleasableCommit -Subject $subject -Paths @($commit.Paths)) {
                    $other.Add($line)
                }
            }
        }
    }
    $blocks = New-Object System.Collections.Generic.List[string]
    if ($added.Count) {
        $blocks.Add("### Added")
        $blocks.Add("")
        $blocks.AddRange($added)
        $blocks.Add("")
    }
    if ($fixed.Count) {
        $blocks.Add("### Fixed")
        $blocks.Add("")
        $blocks.AddRange($fixed)
        $blocks.Add("")
    }
    if ($perf.Count) {
        $blocks.Add("### Performance")
        $blocks.Add("")
        $blocks.AddRange($perf)
        $blocks.Add("")
    }
    if ($other.Count) {
        $blocks.Add("### Other")
        $blocks.Add("")
        $blocks.AddRange($other)
        $blocks.Add("")
    }
    if ($blocks.Count -eq 0) {
        $blocks.Add("No conventional feat/fix/perf subjects were found. Review the")
        $blocks.Add("commit range before publishing.")
        $blocks.Add("")
    }
    $names = Get-SpaceLensArtifactNames $v
    return @"
# SpaceLens $($v.Tag)

$($prev.Tag) stays published and immutable. This is a new tag, new assets, and
``@tungcorn/spacelens@$($v.Text)``.

<!-- BEGIN GENERATED NOTES -->
## What changed

$($blocks -join "`n")
<!-- END GENERATED NOTES -->

## Safety

CLI and MCP analysis do not mutate analyzed user files.
``filesystem_mutation: false`` and ``read_only: true``. Human-authorized
Recycle Bin maintenance remains GUI-only.

**AI recommendation is not filesystem permission.**

MCP still exposes six read-only tools. This release does not add
delete, recycle, restore, move, or index-refresh tools.

## Downloads

| Asset | Use |
| --- | --- |
| [$($names.Unified)](https://github.com/tungcorn/spacelens/releases/download/$($v.Tag)/$($names.Unified)) | Recommended: GUI + CLI + MCP (Qt 6.8.3 runtime included) |
| [$($names.Headless)](https://github.com/tungcorn/spacelens/releases/download/$($v.Tag)/$($names.Headless)) | Headless: CLI + MCP, no GUI, no Qt |
| [$($names.Sums)](https://github.com/tungcorn/spacelens/releases/download/$($v.Tag)/$($names.Sums)) | SHA-256 of the attached zips |

Do not install both archives together. Both expose ``spacelens``.

Developer / terminal-friendly install (Windows x64):

``````text
npm install -g @tungcorn/spacelens
``````
"@
}

function Update-SpaceLensGeneratedRegion {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$BeginMarker,
        [Parameter(Mandatory = $true)][string]$EndMarker,
        [Parameter(Mandatory = $true)][string]$Replacement
    )
    $ordinal = [System.StringComparison]::Ordinal
    $start = $Text.IndexOf($BeginMarker, $ordinal)
    if ($start -lt 0) { return $null }
    $searchFrom = $start + $BeginMarker.Length
    if ($searchFrom -gt $Text.Length) { return $null }
    $stop = $Text.IndexOf($EndMarker, $searchFrom, $ordinal)
    if ($stop -lt 0) { return $null }
    $endExclusive = $stop + $EndMarker.Length
    $prefix = if ($start -gt 0) { $Text.Substring(0, $start) } else { "" }
    $suffix = if ($endExclusive -lt $Text.Length) { $Text.Substring($endExclusive) } else { "" }
    return $prefix + $Replacement.TrimEnd() + "`n" + $suffix
}

function Format-SpaceLensChangelogSection {
    param(
        [Parameter(Mandatory = $true)]$Version,
        [Parameter(Mandatory = $true)][string]$Date,
        [Parameter(Mandatory = $true)][string]$GeneratedBody
    )
    $v = if ($Version -is [string]) { ConvertTo-SpaceLensVersion $Version } else { $Version }
    return @"
## [$($v.Text)] — $Date

https://github.com/tungcorn/spacelens/releases/tag/$($v.Tag)

<!-- BEGIN GENERATED CHANGELOG $($v.Text) -->
$GeneratedBody
<!-- END GENERATED CHANGELOG $($v.Text) -->
"@
}

function Format-SpaceLensDryRun {
    param(
        [Parameter(Mandatory = $true)]$Decision,
        [string]$Head = "HEAD"
    )
    $latest = $Decision.LatestVersion.Text
    $next = if ($Decision.NextVersion) { $Decision.NextVersion.Text } else { "(none)" }
    $tag = if ($Decision.NextVersion) { $Decision.NextVersion.Tag } else { "(none)" }
    $names = $null
    if ($Decision.NextVersion) {
        $names = Get-SpaceLensArtifactNames $Decision.NextVersion
    }
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("latest_release=$latest")
    $lines.Add("head=$Head")
    $lines.Add("releasable=$($Decision.Needed.ToString().ToLowerInvariant())")
    $lines.Add("proposed_version=$next")
    $lines.Add("intended_tag=$tag")
    if ($names) {
        $lines.Add("unified_zip=$($names.Unified)")
        $lines.Add("headless_zip=$($names.Headless)")
        $lines.Add("checksums=$($names.Sums)")
    } else {
        $lines.Add("unified_zip=(none)")
        $lines.Add("headless_zip=(none)")
        $lines.Add("checksums=(none)")
    }
    $lines.Add("releasable_commits=$($Decision.ReleasableCount)")
    foreach ($commit in $Decision.Releasable) {
        $short = if ($commit.Hash) { $commit.Hash.Substring(0, [Math]::Min(7, $commit.Hash.Length)) } else { "-------" }
        $lines.Add("  $short $($commit.Subject)")
    }
    if ($Decision.Needed) {
        $lines.Add("files_that_would_change=")
        foreach ($rel in Get-SpaceLensMechanicalVersionFiles) {
            $lines.Add("  $rel")
        }
        $lines.Add("  CHANGELOG.md")
        $lines.Add("  docs/release-notes/$tag.md")
    }
    return ($lines -join [Environment]::NewLine)
}
