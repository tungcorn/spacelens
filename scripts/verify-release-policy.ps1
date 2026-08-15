# Offline tests for Release Automation V1 policy. No network. No tags created.

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "SpaceLensRelease.ps1")

$failures = New-Object System.Collections.Generic.List[string]
function Add-Fail([string]$Message) {
    $script:failures.Add($Message)
    Write-Host "POLICY FAIL: $Message"
}

function New-Commit([string]$Subject, [string[]]$Paths = @()) {
    return [pscustomobject]@{ Hash = "deadbeef"; Subject = $Subject; Paths = $Paths }
}

# Semver: 0.1.9 < 0.1.10, prerelease suffix ignored.
$v19 = ConvertTo-SpaceLensVersion "v0.1.9"
$v110 = ConvertTo-SpaceLensVersion "0.1.10"
if (-not $v19 -or -not $v110) { Add-Fail "failed to parse 0.1.9 / 0.1.10" }
elseif ((Compare-SpaceLensVersion $v19 $v110) -ge 0) { Add-Fail "0.1.9 must be older than 0.1.10" }
if (ConvertTo-SpaceLensVersion "v0.1.5-rc.1") { Add-Fail "prerelease tag must not parse as stable" }
if (ConvertTo-SpaceLensVersion "v1.0") { Add-Fail "incomplete version must not parse" }

$latest = Get-SpaceLensLatestStableVersion -Tags @("v0.1.9", "v0.1.10", "v0.1.5-rc.1", "not-a-tag", "v0.1.4")
if (-not $latest -or $latest.Text -ne "0.1.10") { Add-Fail "latest stable tag should be 0.1.10, got $($latest.Text)" }

$next = Get-SpaceLensNextPatchVersion "0.1.4"
if ($next.Text -ne "0.1.5" -or $next.Tag -ne "v0.1.5") { Add-Fail "next patch of 0.1.4 must be 0.1.5" }

# Scenario A: docs-only → no release
$a = Get-SpaceLensReleaseDecision -LatestVersion "0.1.4" -Commits @(
    (New-Commit "docs: fix typo" @("README.md")),
    (New-Commit "test: add coverage" @("tests/test_foo.cpp"))
)
if ($a.Needed) { Add-Fail "A docs-only range must not require a release" }

# Scenario B: feat → pending 0.1.5
$b = Get-SpaceLensReleaseDecision -LatestVersion "0.1.4" -Commits @(
    (New-Commit "feat(index): catalog roots" @("src/core/index/IndexCatalog.cpp"))
)
if (-not $b.Needed -or $b.NextVersion.Text -ne "0.1.5") { Add-Fail "B feat must pending 0.1.5" }

# Scenario C: feat+test+fix+docs → one 0.1.5
$c = Get-SpaceLensReleaseDecision -LatestVersion "0.1.4" -Commits @(
    (New-Commit "feat(core): rank v2" @("src/core/StorageIntelligence.cpp")),
    (New-Commit "test: rank fixtures" @("tests/test_rank.cpp")),
    (New-Commit "fix(index): drive-root under" @("src/core/index/IndexQuery.cpp")),
    (New-Commit "docs: rank contract" @("docs/INDEX.md"))
)
if (-not $c.Needed -or $c.NextVersion.Text -ne "0.1.5" -or $c.ReleasableCount -ne 2) {
    Add-Fail "C must be one pending 0.1.5 with 2 releasable commits, got needed=$($c.Needed) ver=$($c.NextVersion.Text) n=$($c.ReleasableCount)"
}

# Scenario D: pin chore → no release
$d = Get-SpaceLensReleaseDecision -LatestVersion "0.1.4" -Commits @(
    (New-Commit "chore(npm): pin pack-from-release to public v0.1.4 hash" @("packaging/npm/release-pin.env", "docs/RELEASING.md"))
)
if ($d.Needed) { Add-Fail "D pin commit must not require a release" }

# Scenario E: perf → release
$e = Get-SpaceLensReleaseDecision -LatestVersion "0.1.4" -Commits @(
    (New-Commit "perf(index): stream aggregates" @("src/core/index/IndexQuery.cpp"))
)
if (-not $e.Needed) { Add-Fail "E perf must require a release" }

# Scenario F: more product commits still one pending next patch (not 0.1.6).
$f = Get-SpaceLensReleaseDecision -LatestVersion "0.1.4" -Commits @(
    (New-Commit "feat(index): catalog roots" @("src/core/index/IndexCatalog.cpp")),
    (New-Commit "feat(cli): print catalog count" @("src/cli/Commands.cpp")),
    (New-Commit "docs: catalog" @("docs/INDEX.md"))
)
if (-not $f.Needed -or $f.NextVersion.Text -ne "0.1.5") {
    Add-Fail "F growing range must stay one pending 0.1.5"
}

# Release prep must not loop.
$prep = Get-SpaceLensReleaseDecision -LatestVersion "0.1.4" -Commits @(
    (New-Commit "chore(release): prepare SpaceLens v0.1.5" @("CMakeLists.txt", "CHANGELOG.md")),
    (New-Commit "release: prepare SpaceLens v0.1.4" @("CMakeLists.txt"))
)
if ($prep.Needed) { Add-Fail "release-prep subjects must not require another release" }

# Mislabeled product change fails safe (treat as releasable).
$mis = Get-SpaceLensReleaseDecision -LatestVersion "0.1.4" -Commits @(
    (New-Commit "docs: tweak analyzer" @("src/core/StorageAnalysis.cpp"))
)
if (-not $mis.Needed) { Add-Fail "docs: commit that touches src/core must fail-safe to releasable" }

# ci/docs scripts are not product paths.
$ciOnly = Get-SpaceLensReleaseDecision -LatestVersion "0.1.4" -Commits @(
    (New-Commit "ci: automate patch releases" @(".github/workflows/release.yml", "scripts/SpaceLensRelease.ps1")),
    (New-Commit "docs: document automated releases" @("docs/RELEASING.md", "CONTRIBUTING.md"))
)
if ($ciOnly.Needed) { Add-Fail "ci/docs release-automation commits must not require a product release" }

# Auto-publish only the version-bump commit (or a tagged retry), not later SHAs.
if (-not (Test-SpaceLensAutoPublishCommit -CMakeVersion "0.1.5" -LatestStableVersion "0.1.4" -ParentCMakeVersion "0.1.4" -HeadSha "aaa")) {
    Add-Fail "bump commit must auto-publish"
}
if (Test-SpaceLensAutoPublishCommit -CMakeVersion "0.1.5" -LatestStableVersion "0.1.4" -ParentCMakeVersion "0.1.5" -HeadSha "bbb") {
    Add-Fail "later SHA while CMake is already next must not auto-publish"
}
if (-not (Test-SpaceLensAutoPublishCommit -CMakeVersion "0.1.5" -LatestStableVersion "0.1.4" -ParentCMakeVersion "0.1.5" -HeadSha "ccc" -TagPeelSha "ccc")) {
    Add-Fail "retry of the already-tagged SHA must auto-publish"
}
if (Test-SpaceLensAutoPublishCommit -CMakeVersion "0.1.4" -LatestStableVersion "0.1.4" -ParentCMakeVersion "0.1.4" -HeadSha "ddd") {
    Add-Fail "unbumped CMake must not auto-publish"
}

# Publish plan G: rerun after public release exists → no duplicate.
$g = Get-SpaceLensPublishPlan -PreparedVersion "0.1.5" -LatestStableVersion "0.1.4" `
    -TagPeelSha "abc" -HeadSha "abc" -GitHubReleaseExists $true -NpmVersionExists $true `
    -CurrentPinVersion "0.1.5" -CurrentPinSha256 ("a" * 64) -PublicUnifiedSha256 ("a" * 64)
if (-not $g.AlreadyComplete) { Add-Fail "G complete public release must be AlreadyComplete" }
if ($g.CreateTag -or $g.CreateRelease -or $g.PublishNpm -or $g.UpdatePin) {
    Add-Fail "G rerun must not create tag/release/npm/pin"
}

# Publish plan: refuse to move a tag.
$moved = Get-SpaceLensPublishPlan -PreparedVersion "0.1.5" -LatestStableVersion "0.1.4" `
    -TagPeelSha "old" -HeadSha "new"
if (-not $moved.Refuse) { Add-Fail "must refuse to move an existing tag" }

# H: pin already current → no extra commit.
$h = Get-SpaceLensPublishPlan -PreparedVersion "0.1.5" -LatestStableVersion "0.1.4" `
    -TagPeelSha "abc" -HeadSha "abc" -GitHubReleaseExists $true -NpmVersionExists $true `
    -CurrentPinVersion "0.1.5" -CurrentPinSha256 ("b" * 64) -PublicUnifiedSha256 ("b" * 64)
if ($h.UpdatePin) { Add-Fail "H matching pin must not update" }

$hNeed = Get-SpaceLensPublishPlan -PreparedVersion "0.1.5" -LatestStableVersion "0.1.4" `
    -TagPeelSha "abc" -HeadSha "abc" -GitHubReleaseExists $true -NpmVersionExists $true `
    -CurrentPinVersion "0.1.4" -CurrentPinSha256 ("b" * 64) -PublicUnifiedSha256 ("c" * 64)
if (-not $hNeed.UpdatePin) { Add-Fail "pin must update when public hash differs" }

# Artifact names stay stable.
$names = Get-SpaceLensArtifactNames "0.1.5"
if ($names.Unified -ne "spacelens-v0.1.5-windows-x64.zip") { Add-Fail "unified zip name changed" }
if ($names.Headless -ne "spacelens-cli-v0.1.5-windows-x64.zip") { Add-Fail "headless zip name changed" }
if ($names.Sums -ne "SHA256SUMS.txt") { Add-Fail "checksum name changed" }

# Pin file generation.
$pin = New-SpaceLensReleasePinText -Version "0.1.5" -Sha256 ("d" * 64)
if ($pin -notmatch "SPACELENS_VERSION=0.1.5") { Add-Fail "pin missing version" }
if ($pin -notmatch ("RELEASE_SHA256=" + ("d" * 64))) { Add-Fail "pin missing hash" }
if ($pin -notmatch "RELEASE_ASSET=spacelens-v0.1.5-windows-x64.zip") { Add-Fail "pin missing asset" }
try {
    New-SpaceLensReleasePinText -Version "0.1.5" -Sha256 "nope" | Out-Null
    Add-Fail "pin generator must reject a short hash"
} catch { }

# Loop subject helper.
if (-not (Test-SpaceLensReleaseLoopSubject "chore(npm): pin pack-from-release to public v0.1.4 hash")) {
    Add-Fail "pin subject should be a loop exclusion"
}
if (-not (Test-SpaceLensReleasePrepSubject "release: prepare SpaceLens v0.1.4" -ExpectedVersion "0.1.4")) {
    Add-Fail "historical v0.1.4 prep subject must match"
}
if (Test-SpaceLensReleasePrepSubject "feat(index): prepare SpaceLens v0.1.5") {
    Add-Fail "feat subject must not count as release prep"
}

# Live classifier must be internally consistent (do not freeze 0.1.4 here).
$root = Get-SpaceLensRepoRoot
$repoLatest = Get-SpaceLensLatestStableVersion -Tags (Get-SpaceLensGitTags -RepoRoot $root)
if (-not $repoLatest) {
    Add-Fail "repository has no stable vX.Y.Z tag"
} else {
    $live = Get-SpaceLensCommitsSinceTag -Tag $repoLatest.Tag -Head "HEAD" -RepoRoot $root
    $liveDecision = Get-SpaceLensReleaseDecision -Commits $live -LatestVersion $repoLatest
    if ($liveDecision.Needed) {
        $expected = Get-SpaceLensNextPatchVersion $repoLatest
        if ($liveDecision.NextVersion.Text -ne $expected.Text) {
            Add-Fail "live next version must be the next patch ($($expected.Text))"
        }
    }
}

# Pin updater: matching fixture does not rewrite; mismatch would update.
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-policy-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    $fakeZip = Join-Path $scratch "spacelens-v0.1.5-windows-x64.zip"
    $payload = [byte[]](1..64)
    [System.IO.File]::WriteAllBytes($fakeZip, $payload)
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $fakeZip).Hash.ToLowerInvariant()
    $sums = Join-Path $scratch "SHA256SUMS.txt"
    Set-Content -LiteralPath $sums -Value "$hash  spacelens-v0.1.5-windows-x64.zip" -Encoding ascii
    $fakeRoot = Join-Path $scratch "repo"
    New-Item -ItemType Directory -Path (Join-Path $fakeRoot "packaging\npm") | Out-Null
    $existingPin = New-SpaceLensReleasePinText -Version "0.1.5" -Sha256 $hash
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText((Join-Path $fakeRoot "packaging\npm\release-pin.env"), $existingPin + "`n", $utf8)
    & (Join-Path $PSScriptRoot "update-release-pin.ps1") -Version 0.1.5 -RepoRoot $fakeRoot -ZipPath $fakeZip -SumsPath $sums -WriteFiles
    if ($LASTEXITCODE -ne 0) { Add-Fail "matching pin fixture should exit 0" }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

# Mechanical version rewrite on a temp tree.
$prepRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-prep-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $prepRoot | Out-Null
try {
    $mech = @(Get-SpaceLensMechanicalVersionFiles)
    foreach ($rel in $mech) {
        if ($rel -match '(?i)(?:^|/)\.github/workflows/') {
            Add-Fail "mechanical version files must not include $rel"
        }
    }
    $requiredSources = @(
        "CMakeLists.txt",
        "packaging/npm/package.json",
        "src/cli/main.cpp",
        "src/cli/Commands.cpp",
        "src/mcp/Protocol.hpp",
        "src/app/Application.cpp",
        "src/core/StorageAnalysis.cpp"
    )
    foreach ($rel in $requiredSources) {
        if ($rel -notin $mech) {
            Add-Fail "mechanical version files missing required product source $rel"
        }
    }
    foreach ($rel in $mech) {
        $dest = Join-Path $prepRoot ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
        $dir = Split-Path -Parent $dest
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
        $src = Join-Path $root ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
        Copy-Item -LiteralPath $src -Destination $dest
    }
    $workflowRels = @(
        ".github/workflows/release.yml",
        ".github/workflows/npm-publish.yml"
    )
    $workflowBefore = @{}
    foreach ($rel in $workflowRels) {
        $dest = Join-Path $prepRoot ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
        $dir = Split-Path -Parent $dest
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
        $src = Join-Path $root ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
        Copy-Item -LiteralPath $src -Destination $dest
        $workflowBefore[$rel] = Get-Content -LiteralPath $dest -Raw
    }
    $from = Get-SpaceLensCMakeVersion -RepoRoot $prepRoot
    $to = Get-SpaceLensNextPatchVersion $from
    $changed = @(Update-SpaceLensMechanicalVersions -RepoRoot $prepRoot -FromVersion $from.Text -ToVersion $to.Text)
    foreach ($rel in $workflowRels) {
        if ($rel -in $changed) {
            Add-Fail "preparing $($to.Text) reported a change to $rel"
        }
        $after = Get-Content -LiteralPath (Join-Path $prepRoot ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)) -Raw
        if ($after -ne $workflowBefore[$rel]) {
            Add-Fail "preparing $($to.Text) modified $rel"
        }
    }
    foreach ($rel in $requiredSources) {
        if ($rel -notin $changed) {
            Add-Fail "preparing $($to.Text) did not bump required product source $rel"
        }
        $text = Get-Content -LiteralPath (Join-Path $prepRoot ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)) -Raw
        if ($text -notmatch [regex]::Escape($to.Text)) {
            Add-Fail "$rel was not bumped to $($to.Text)"
        }
        if ($text -match [regex]::Escape($from.Text)) {
            Add-Fail "$rel still contains $($from.Text)"
        }
    }
    $cmake = Get-Content (Join-Path $prepRoot "CMakeLists.txt") -Raw
    if ($cmake -notmatch "VERSION $($to.Text)") { Add-Fail "temp CMake was not bumped to $($to.Text)" }
    if ($cmake -match "project\(\s*SpaceLens\s+VERSION $($from.Text)") {
        Add-Fail "temp CMake still has project VERSION $($from.Text)"
    }
    $pkg = Get-Content (Join-Path $prepRoot "packaging\npm\package.json") -Raw
    if ($pkg -notmatch "`"version`": `"$($to.Text)`"") { Add-Fail "temp package.json was not bumped" }

    $human = "# SpaceLens $($to.Tag)`n`nHuman intro.`n`n<!-- BEGIN GENERATED NOTES -->`nold`n<!-- END GENERATED NOTES -->`n`nHuman footer.`n"
    $updated = Update-SpaceLensGeneratedRegion -Text $human -BeginMarker "<!-- BEGIN GENERATED NOTES -->" -EndMarker "<!-- END GENERATED NOTES -->" -Replacement "<!-- BEGIN GENERATED NOTES -->`nnew`n<!-- END GENERATED NOTES -->"
    if ($updated -notmatch 'Human intro' -or $updated -notmatch 'Human footer' -or $updated -notmatch '(?s)BEGIN GENERATED NOTES -->\s*new\s*<!-- END') {
        Add-Fail "generated notes region must overwrite inside markers only"
    }
    $untouched = Update-SpaceLensGeneratedRegion -Text "# no markers`n" -BeginMarker "<!-- BEGIN GENERATED NOTES -->" -EndMarker "<!-- END GENERATED NOTES -->" -Replacement "x"
    if ($null -ne $untouched) { Add-Fail "notes without markers must be left alone" }
} finally {
    Remove-Item -LiteralPath $prepRoot -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host "verify-release-policy: $($failures.Count) failure(s)"
    exit 1
}
Write-Host "verify-release-policy: PASS"
exit 0
