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

# Multiline CMake project(VERSION) must parse (already_bumped cannot require one line).
$multiCmake = @"
project(SpaceLens
    VERSION 0.1.5
    DESCRIPTION "native"
    LANGUAGES CXX
)
"@
try {
    $parsedMulti = Get-SpaceLensCMakeVersionFromText $multiCmake
    if ($parsedMulti.Text -ne "0.1.5") { Add-Fail "multiline CMake must parse as 0.1.5" }
} catch {
    Add-Fail "multiline CMake project(VERSION) must parse: $($_.Exception.Message)"
}

# Event matrix A–I (offline). Real defects stay red; expected no-ops do not prepare/publish.
$wfFeatSuccess = Get-SpaceLensAutomationIntent -EventName "workflow_run" -WorkflowRunConclusion "success" `
    -WorkflowRunBranch "main" -WorkflowRunEvent "push" -Subject "feat(index): catalog roots" `
    -Paths @("src/core/index/IndexCatalog.cpp") -CMakeVersion "0.1.5" -LatestStableVersion "0.1.5"
if (-not $wfFeatSuccess.PrepareNeeded -or $wfFeatSuccess.RunPipeline -or $wfFeatSuccess.NextVersion.Text -ne "0.1.6") {
    Add-Fail "Matrix A: workflow_run with CI success on feature push must prepare next patch 0.1.6"
}

$wfFeatFail = Get-SpaceLensAutomationIntent -EventName "workflow_run" -WorkflowRunConclusion "failure" `
    -WorkflowRunBranch "main" -WorkflowRunEvent "push" -Subject "feat(index): catalog roots" `
    -Paths @("src/core/index/IndexCatalog.cpp") -CMakeVersion "0.1.5" -LatestStableVersion "0.1.5"
if ($wfFeatFail.PrepareNeeded -or $wfFeatFail.RunPipeline) {
    Add-Fail "Matrix B: workflow_run with CI failure must NOT prepare or run pipeline"
}

$wfDocs = Get-SpaceLensAutomationIntent -EventName "workflow_run" -WorkflowRunConclusion "success" `
    -WorkflowRunBranch "main" -WorkflowRunEvent "push" -Subject "docs: tighten README" `
    -Paths @("README.md") -CMakeVersion "0.1.5" -LatestStableVersion "0.1.5"
if ($wfDocs.RunPipeline -or $wfDocs.PrepareNeeded -or $wfDocs.RunExpensiveCi -or -not $wfDocs.DocsOnly) {
    Add-Fail "Matrix C: docs-only workflow_run must be cheap CI, no prepare, no publish"
}

$wfChore = Get-SpaceLensAutomationIntent -EventName "workflow_run" -WorkflowRunConclusion "success" `
    -WorkflowRunBranch "main" -WorkflowRunEvent "push" -Subject "ci: harden automatic release workflow" `
    -Paths @(".github/workflows/release.yml", "scripts/SpaceLensRelease.ps1") `
    -CMakeVersion "0.1.5" -LatestStableVersion "0.1.5"
if ($wfChore.RunPipeline -or $wfChore.PrepareNeeded) {
    Add-Fail "Matrix D: ci/chore-only workflow_run must not prepare or publish"
}

$wfBumpCi = Get-SpaceLensAutomationIntent -EventName "workflow_run" -WorkflowRunConclusion "success" `
    -WorkflowRunBranch "main" -WorkflowRunEvent "workflow_dispatch" -Subject "chore(release): prepare SpaceLens v0.1.6" `
    -Paths @("CMakeLists.txt") -CMakeVersion "0.1.6" -LatestStableVersion "0.1.5" -ParentCMakeVersion "0.1.5" -HeadSha "bumpsha"
if ($wfBumpCi.PrepareNeeded) {
    Add-Fail "Matrix E: workflow_run triggered by workflow_dispatch CI must NOT start release prepare"
}

$docsPush = Get-SpaceLensAutomationIntent -EventName "push" -Subject "docs: tighten README" `
    -Paths @("README.md") -CMakeVersion "0.1.5" -LatestStableVersion "0.1.5"
if ($docsPush.RunPipeline -or $docsPush.PrepareNeeded -or $docsPush.RunExpensiveCi -or -not $docsPush.DocsOnly) {
    Add-Fail "A docs-only push must be cheap CI, no prepare, no publish"
}

$chorePush = Get-SpaceLensAutomationIntent -EventName "push" -Subject "ci: harden automatic release workflow" `
    -Paths @(".github/workflows/release.yml", "scripts/SpaceLensRelease.ps1") `
    -CMakeVersion "0.1.5" -LatestStableVersion "0.1.5"
if ($chorePush.RunPipeline -or $chorePush.PrepareNeeded) {
    Add-Fail "B ci/chore-only push must not prepare or publish"
}
if (-not $chorePush.RunExpensiveCi) {
    Add-Fail "B workflow/script changes must still run expensive CI"
}

$featPush = Get-SpaceLensAutomationIntent -EventName "push" -Subject "feat(index): catalog roots" `
    -Paths @("src/core/index/IndexCatalog.cpp") -CMakeVersion "0.1.5" -LatestStableVersion "0.1.5"
if (-not $featPush.PrepareNeeded -or $featPush.RunPipeline -or $featPush.NextVersion.Text -ne "0.1.6") {
    Add-Fail "C one feat push must prepare exactly one next patch and not publish yet"
}

$multiFeat = Get-SpaceLensAutomationIntent -EventName "push" -Subject "feat(cli): print count" `
    -Paths @("src/cli/Commands.cpp") -CMakeVersion "0.1.5" -LatestStableVersion "0.1.5" `
    -RangeCommits @(
        (New-Commit "feat(index): catalog roots" @("src/core/index/IndexCatalog.cpp")),
        (New-Commit "fix(index): drive-root under" @("src/core/index/IndexQuery.cpp")),
        (New-Commit "feat(cli): print count" @("src/cli/Commands.cpp"))
    )
if (-not $multiFeat.PrepareNeeded -or $multiFeat.NextVersion.Text -ne "0.1.6") {
    Add-Fail "D multiple feat/fix commits in one push must still be one patch"
}

$bumpPush = Get-SpaceLensAutomationIntent -EventName "push" `
    -Subject "chore(release): prepare SpaceLens v0.1.6" `
    -Paths @("CMakeLists.txt", "packaging/npm/package.json", "CHANGELOG.md") `
    -CMakeVersion "0.1.6" -LatestStableVersion "0.1.5" -ParentCMakeVersion "0.1.5" `
    -HeadSha "bumpsha"
if (-not $bumpPush.RunPipeline -or -not $bumpPush.Publish -or $bumpPush.PrepareNeeded -or -not $bumpPush.LoopExcluded) {
    Add-Fail "E generated bump must publish and must not recursively prepare"
}
if ($bumpPush.EnsureCi -ne "dispatch") {
    Add-Fail "E bump publish without existing CI must dispatch CI once"
}
$bumpReuse = Get-SpaceLensAutomationIntent -EventName "push" `
    -Subject "chore(release): prepare SpaceLens v0.1.6" `
    -Paths @("CMakeLists.txt") -CMakeVersion "0.1.6" -LatestStableVersion "0.1.5" `
    -ParentCMakeVersion "0.1.5" -HeadSha "bumpsha" `
    -ExistingCiRuns @([pscustomobject]@{ status = "in_progress"; conclusion = "" })
if ($bumpReuse.EnsureCi -ne "reuse") {
    Add-Fail "E must not dispatch a second CI run when one is already in progress"
}

$pinPush = Get-SpaceLensAutomationIntent -EventName "push" `
    -Subject "chore(npm): pin pack-from-release to public v0.1.5 hash" `
    -Paths @("packaging/npm/release-pin.env", "docs/RELEASING.md") `
    -CMakeVersion "0.1.5" -LatestStableVersion "0.1.5"
if ($pinPush.RunPipeline -or $pinPush.PrepareNeeded -or $pinPush.DispatchCiAfterPin -or -not $pinPush.PinOnly -or $pinPush.RunExpensiveCi) {
    Add-Fail "F pin commit must not release and must not run expensive CI"
}

$recovery = Get-SpaceLensAutomationIntent -EventName "workflow_dispatch" `
    -Subject "chore(release): prepare SpaceLens v0.1.5" `
    -Paths @("CMakeLists.txt") -CMakeVersion "0.1.5" -LatestStableVersion "0.1.5" `
    -PublishInput "true" -HeadSha "pubsha" -TagPeelSha "pubsha" `
    -ExistingCiRuns @([pscustomobject]@{ status = "completed"; conclusion = "success" })
if (-not $recovery.RunPipeline -or -not $recovery.Publish -or $recovery.PrepareNeeded) {
    Add-Fail "G recovery dispatch must run the publish pipeline without preparing another bump"
}
if ($recovery.EnsureCi -ne "reuse") {
    Add-Fail "G recovery must reuse successful CI on the already-published SHA"
}

$gPlan = Get-SpaceLensPublishPlan -PreparedVersion "0.1.5" -LatestStableVersion "0.1.4" `
    -TagPeelSha "abc" -HeadSha "abc" -GitHubReleaseExists $true -NpmVersionExists $true `
    -CurrentPinVersion "0.1.5" -CurrentPinSha256 ("a" * 64) -PublicUnifiedSha256 ("a" * 64)
if (-not $gPlan.AlreadyComplete -or $gPlan.CreateTag -or $gPlan.CreateRelease -or $gPlan.PublishNpm) {
    Add-Fail "G already-published rerun must be idempotent"
}

if ((Get-SpaceLensEnsureCiDecision -Runs @()) -ne "dispatch") {
    Add-Fail "H missing CI must dispatch (recovery without a PR)"
}
$moved = Get-SpaceLensPublishPlan -PreparedVersion "0.1.5" -LatestStableVersion "0.1.4" `
    -TagPeelSha "old" -HeadSha "new"
if (-not $moved.Refuse) { Add-Fail "H must refuse to move an existing tag" }

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

# Missing next tag is the normal new-release path (empty ls-remote must not throw).
foreach ($emptyRemote in @($null, "", @(), "   ", @("", "  "))) {
    try {
        $emptySha = Get-SpaceLensLsRemoteSha -Output $emptyRemote
        if ($emptySha) {
            Add-Fail "empty ls-remote must yield empty sha, got '$emptySha'"
        }
    } catch {
        Add-Fail "empty ls-remote must not throw: $($_.Exception.Message)"
    }
}
$presentSha = Get-SpaceLensLsRemoteSha -Output "abc123def456`trefs/tags/v0.1.6"
if ($presentSha -ne "abc123def456") {
    Add-Fail "present ls-remote must parse the object SHA, got '$presentSha'"
}
try {
    $missingLookup = Get-SpaceLensPrepareIdempotency -MainCMakeVersion "0.1.5" -NextVersion "0.1.6" -TagSha ""
    if ($missingLookup.TagExists) { Add-Fail "missing v0.1.6 must be tag_exists=false" }
    if ($missingLookup.AlreadyBumped) { Add-Fail "CMake 0.1.5 vs next 0.1.6 must not be already_bumped" }
    if (-not $missingLookup.ContinuePrepare) { Add-Fail "missing v0.1.6 must continue release preparation" }
} catch {
    Add-Fail "missing v0.1.6 prepare check must not throw: $($_.Exception.Message)"
}
$presentLookup = Get-SpaceLensPrepareIdempotency -MainCMakeVersion "0.1.5" -NextVersion "0.1.6" -TagSha $presentSha
if (-not $presentLookup.TagExists) { Add-Fail "present v0.1.6 must be tag_exists=true" }
if ($presentLookup.ContinuePrepare) { Add-Fail "existing next tag must skip prepare" }
$expectedBump = Get-SpaceLensPrepareIdempotency -MainCMakeVersion "0.1.6" -NextVersion "0.1.6" `
    -TagSha "" -MainSha "bumpsha" -MainSubject "chore(release): prepare SpaceLens v0.1.6"
if (-not $expectedBump.AlreadyBumped -or $expectedBump.TagExists -or $expectedBump.BumpSha -ne "bumpsha") {
    Add-Fail "already-bumped missing tag must recover the bump SHA"
}

# Isolated git repo: refs/tags/v0.1.6 does not exist → no exception, tag_exists=false.
$tagScratch = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-taglookup-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tagScratch | Out-Null
try {
    & git -C $tagScratch init --quiet
    if ($LASTEXITCODE -ne 0) { throw "git init failed" }
    & git -C $tagScratch config user.email "release-test@example.com"
    & git -C $tagScratch config user.name "SpaceLens Release Test"
    & git -C $tagScratch -c commit.gpgsign=false commit --allow-empty -m "init" --quiet
    if ($LASTEXITCODE -ne 0) { throw "git commit failed" }
    $remoteMissing = Get-SpaceLensRemoteTagSha -Tag "v0.1.6" -Remote $tagScratch
    if ($remoteMissing) {
        Add-Fail "isolated repo without v0.1.6 must return empty remote tag sha, got '$remoteMissing'"
    }
    $remoteMissingCheck = Get-SpaceLensPrepareIdempotency -MainCMakeVersion "0.1.5" -NextVersion "0.1.6" -TagSha $remoteMissing
    if ($remoteMissingCheck.TagExists -or -not $remoteMissingCheck.ContinuePrepare) {
        Add-Fail "isolated missing v0.1.6 must continue release preparation"
    }
    & git -C $tagScratch tag -a "v0.1.6" -m "SpaceLens v0.1.6"
    if ($LASTEXITCODE -ne 0) { throw "git tag failed" }
    $remotePresent = Get-SpaceLensRemoteTagSha -Tag "v0.1.6" -Remote $tagScratch
    if ([string]::IsNullOrWhiteSpace($remotePresent)) {
        Add-Fail "isolated repo with v0.1.6 must return a remote tag sha"
    }
    $remotePresentCheck = Get-SpaceLensPrepareIdempotency -MainCMakeVersion "0.1.5" -NextVersion "0.1.6" -TagSha $remotePresent
    if (-not $remotePresentCheck.TagExists) { Add-Fail "isolated present v0.1.6 must be tag_exists=true" }
    if ($remotePresentCheck.ContinuePrepare) { Add-Fail "isolated present v0.1.6 must skip prepare" }
    $headSha = (& git -C $tagScratch rev-parse "HEAD").Trim()
    $unexpected = Get-SpaceLensPublishPlan -PreparedVersion "0.1.6" -LatestStableVersion "0.1.5" `
        -TagPeelSha "someone-else" -HeadSha $headSha
    if (-not $unexpected.Refuse) { Add-Fail "tag pointing elsewhere must still refuse" }
} catch {
    Add-Fail "isolated missing-tag lookup failed: $($_.Exception.Message)"
} finally {
    Remove-Item -LiteralPath $tagScratch -Recurse -Force -ErrorAction SilentlyContinue
}

# Failed ls-remote is a hard error, not "tag does not exist".
$lsRemoteFailScratch = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-lsremote-fail-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $lsRemoteFailScratch | Out-Null
try {
    & git -C $lsRemoteFailScratch init --quiet
    if ($LASTEXITCODE -ne 0) { throw "git init failed" }
    & git -C $lsRemoteFailScratch config user.email "release-test@example.com"
    & git -C $lsRemoteFailScratch config user.name "SpaceLens Release Test"
    Push-Location $lsRemoteFailScratch
    try {
        Get-SpaceLensRemoteTagSha -Tag "v0.1.6" -Remote "not-a-remote" | Out-Null
        Add-Fail "failed ls-remote must throw rather than look like a missing tag"
    } catch {
        if ("$($_.Exception.Message)" -notmatch 'ls-remote') {
            Add-Fail "failed ls-remote must mention ls-remote, got: $($_.Exception.Message)"
        }
    } finally {
        Pop-Location
    }
} catch {
    Add-Fail "failed-ls-remote fixture setup failed: $($_.Exception.Message)"
} finally {
    Remove-Item -LiteralPath $lsRemoteFailScratch -Recurse -Force -ErrorAction SilentlyContinue
}

# Empty latest-tag..HEAD is a normal no-op, not a binding error.
try {
    $emptyDecision = Get-SpaceLensReleaseDecision -LatestVersion "0.1.6" -Commits @()
    if ($emptyDecision.Needed -or $emptyDecision.ReleasableCount -ne 0 -or $emptyDecision.NextVersion) {
        Add-Fail "empty commit range must be Needed=false with no next version"
    }
} catch {
    Add-Fail "empty commit collection must not throw: $($_.Exception.Message)"
}

$emptyWf = Get-SpaceLensAutomationIntent -EventName "workflow_run" `
    -RangeCommits @() -Subject "" `
    -CMakeVersion "0.1.6" -LatestStableVersion "0.1.6" `
    -HeadSha "abc123abc123abc123abc123abc123abc123abc1" `
    -WorkflowRunConclusion "success" -WorkflowRunBranch "main" -WorkflowRunEvent "push"
if ($emptyWf.RunPipeline -or $emptyWf.PrepareNeeded -or $emptyWf.ReleaseNeeded) {
    Add-Fail "workflow_run with latest tag == HEAD must be a clean no-op"
}

# Isolated git history: empty range, one releasable commit, docs-only, and real git failure.
$rangeScratch = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-empty-range-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $rangeScratch | Out-Null
try {
    & git -C $rangeScratch init --quiet
    if ($LASTEXITCODE -ne 0) { throw "git init failed" }
    & git -C $rangeScratch config user.email "release-test@example.com"
    & git -C $rangeScratch config user.name "SpaceLens Release Test"
    New-Item -ItemType Directory -Path (Join-Path $rangeScratch "packaging\npm") | Out-Null
    Set-Content -LiteralPath (Join-Path $rangeScratch "CMakeLists.txt") -Value "project(SpaceLens VERSION 0.1.6 LANGUAGES CXX)`n" -Encoding ascii
    Set-Content -LiteralPath (Join-Path $rangeScratch "packaging\npm\package.json") -Value '{ "name": "@tungcorn/spacelens", "version": "0.1.6" }' -Encoding ascii
    & git -C $rangeScratch add -- CMakeLists.txt packaging/npm/package.json
    & git -C $rangeScratch -c commit.gpgsign=false commit -m "chore(release): prepare SpaceLens v0.1.6" --quiet
    if ($LASTEXITCODE -ne 0) { throw "git commit failed" }
    & git -C $rangeScratch tag -a "v0.1.6" -m "SpaceLens v0.1.6"
    if ($LASTEXITCODE -ne 0) { throw "git tag failed" }

    $zero = @(Get-SpaceLensCommitsSinceTag -Tag "v0.1.6" -Head "HEAD" -RepoRoot $rangeScratch)
    if ($null -eq $zero) { Add-Fail "empty latest-tag..HEAD range must not be null" }
    if ($zero.Count -ne 0) { Add-Fail "empty latest-tag..HEAD range must have zero commits, got $($zero.Count)" }
    $zeroDecision = Get-SpaceLensReleaseDecision -Commits $zero -LatestVersion "0.1.6"
    if ($zeroDecision.Needed) { Add-Fail "zero commits since latest tag must not require a release" }

    $decideScript = Join-Path $PSScriptRoot "decide-release.ps1"
    $headSha = (& git -C $rangeScratch rev-parse HEAD).Trim()
    $decideOut = & $decideScript -RepoRoot $rangeScratch -EventName "workflow_run" `
        -HeadSha $headSha -WorkflowRunConclusion "success" `
        -WorkflowRunBranch "main" -WorkflowRunEvent "push" *>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Add-Fail "Decide must succeed when latest tag == HEAD, exit $LASTEXITCODE"
    }
    if ($decideOut -notmatch 'decide_run_pipeline=false' -or $decideOut -notmatch 'decide_prepare_needed=false' -or $decideOut -notmatch 'decide_release_needed=false') {
        Add-Fail "Decide must report a clean no-op when latest tag == HEAD"
    }

    Set-Content -LiteralPath (Join-Path $rangeScratch "README.md") -Value "docs only`n" -Encoding ascii
    & git -C $rangeScratch add -- README.md
    & git -C $rangeScratch -c commit.gpgsign=false commit -m "docs: note" --quiet
    $docsRange = @(Get-SpaceLensCommitsSinceTag -Tag "v0.1.6" -Head "HEAD" -RepoRoot $rangeScratch)
    $docsDecision = Get-SpaceLensReleaseDecision -Commits $docsRange -LatestVersion "0.1.6"
    if ($docsDecision.Needed) { Add-Fail "docs-only range after latest tag must not require a release" }

    New-Item -ItemType Directory -Path (Join-Path $rangeScratch "src\core") | Out-Null
    Set-Content -LiteralPath (Join-Path $rangeScratch "src\core\Index.cpp") -Value "int x;`n" -Encoding ascii
    & git -C $rangeScratch add -- src/core/Index.cpp
    & git -C $rangeScratch -c commit.gpgsign=false commit -m "feat(index): add file" --quiet
    $featRange = @(Get-SpaceLensCommitsSinceTag -Tag "v0.1.6" -Head "HEAD" -RepoRoot $rangeScratch)
    if ($featRange.Count -lt 1) { Add-Fail "one releasable commit after latest tag must be visible" }
    $featDecision = Get-SpaceLensReleaseDecision -Commits $featRange -LatestVersion "0.1.6"
    if (-not $featDecision.Needed -or $featDecision.NextVersion.Text -ne "0.1.7") {
        Add-Fail "one feat after latest tag must still pending the next patch"
    }

    try {
        Get-SpaceLensCommitsSinceTag -Tag "v9.9.9" -Head "HEAD" -RepoRoot $rangeScratch | Out-Null
        Add-Fail "unresolvable git history must fail rather than look empty"
    } catch {
        if ("$($_.Exception.Message)" -notmatch 'git log') {
            Add-Fail "unresolvable git history must fail as git log error, got: $($_.Exception.Message)"
        }
    }
} catch {
    Add-Fail "isolated empty-range fixture failed: $($_.Exception.Message)"
} finally {
    Remove-Item -LiteralPath $rangeScratch -Recurse -Force -ErrorAction SilentlyContinue
}

# npm child wait: watch/API observation is not the child conclusion.
$watchOk = Get-SpaceLensWorkflowWatchVerdict -WatchExitCode 0
if ($watchOk.Action -ne 'continue') { Add-Fail "successful gh run watch must continue" }

$childOk = Get-SpaceLensWorkflowWatchVerdict -WatchExitCode 1 -HttpStatus 200 -Status "completed" -Conclusion "success"
if ($childOk.Action -ne 'continue') { Add-Fail "authoritative child SUCCESS must continue after watch failure" }

foreach ($bad in @('failure', 'cancelled', 'timed_out', 'action_required', 'startup_failure', 'skipped')) {
    $childBad = Get-SpaceLensWorkflowWatchVerdict -WatchExitCode 1 -HttpStatus 200 -Status "completed" -Conclusion $bad
    if ($childBad.Action -ne 'fail') {
        Add-Fail "authoritative child $bad must fail"
    }
}

$transient = Get-SpaceLensWorkflowWatchVerdict -WatchExitCode 1 -HttpStatus 502
if ($transient.Action -ne 'retry') { Add-Fail "HTTP 502 observation must retry, not fail the child" }

$notFound = Get-SpaceLensWorkflowWatchVerdict -WatchExitCode 1 -HttpStatus 404
if ($notFound.Action -ne 'fail') { Add-Fail "HTTP 404 observation must fail safely" }

$timeoutVerdict = Get-SpaceLensWorkflowWatchVerdict -WatchExitCode 1 -TimedOut
if ($timeoutVerdict.Action -ne 'fail' -or $timeoutVerdict.Reason -notmatch 'timeout') {
    Add-Fail "watch polling timeout must fail safely"
}

$parsed502 = Get-SpaceLensGitHubApiObservation -Output "gh: HTTP 502: Server Error (https://api.github.com/repos/o/r/actions/runs/1)" -ExitCode 1
if ($parsed502.HttpStatus -ne 502 -or $parsed502.Conclusion) {
    Add-Fail "HTTP 502 watcher error must parse as transient observation, not a child conclusion"
}

$poisonJson = '{"status":"completed","conclusion":"success","head_commit":{"message":"fix: handle HTTP 404 from ls-remote"}}'
$poison = Get-SpaceLensGitHubApiObservation -Output $poisonJson -ExitCode 0
if ($poison.HttpStatus -ne 200 -or $poison.Status -ne "completed" -or $poison.Conclusion -ne "success") {
    Add-Fail "successful API JSON must not treat commit-message HTTP 404 as an API status"
}
$poisonVerdict = Get-SpaceLensWorkflowWatchVerdict -WatchExitCode 1 -HttpStatus $poison.HttpStatus -Status $poison.Status -Conclusion $poison.Conclusion
if ($poisonVerdict.Action -ne 'continue') {
    Add-Fail "successful child JSON containing HTTP 404 text must continue"
}

$clock = [datetime]::SpecifyKind([datetime]"2026-08-16T00:00:00Z", [DateTimeKind]::Utc)
$utc = { $script:clock }
$slept = New-Object System.Collections.Generic.List[int]
$sleep = { param([int]$Seconds) $script:slept.Add($Seconds); $script:clock = $script:clock.AddSeconds([Math]::Max(1, $Seconds)) }

$script:clock = [datetime]::SpecifyKind([datetime]"2026-08-16T00:00:00Z", [DateTimeKind]::Utc)
$okWait = Wait-SpaceLensChildWorkflow -RunId "31931952832" -Repo "tungcorn/spacelens" `
    -WatchCommand { param($id, $repo) 1 } `
    -QueryCommand { param($id, $repo)
        if ($id -ne "31931952832") { throw "must poll the exact run id, got $id" }
        [pscustomobject]@{ HttpStatus = 200; Status = "completed"; Conclusion = "success" }
    } `
    -SleepCommand $sleep -UtcNowCommand $utc
if ($okWait.Action -ne 'continue') {
    Add-Fail "child SUCCESS after watch/API transient must continue"
}

try {
    $script:clock = [datetime]::SpecifyKind([datetime]"2026-08-16T00:00:00Z", [DateTimeKind]::Utc)
    Wait-SpaceLensChildWorkflow -RunId "42" -Repo "tungcorn/spacelens" `
        -WatchCommand { param($id, $repo) 1 } `
        -QueryCommand { [pscustomobject]@{ HttpStatus = 200; Status = "completed"; Conclusion = "failure" } } `
        -SleepCommand $sleep -UtcNowCommand $utc | Out-Null
    Add-Fail "child FAILURE must still fail the parent wait"
} catch {
    if ("$($_.Exception.Message)" -notmatch 'conclusion=failure') {
        Add-Fail "child FAILURE must report the conclusion, got: $($_.Exception.Message)"
    }
}

try {
    $script:clock = [datetime]::SpecifyKind([datetime]"2026-08-16T00:00:00Z", [DateTimeKind]::Utc)
    Wait-SpaceLensChildWorkflow -RunId "42" -Repo "tungcorn/spacelens" `
        -PollTimeoutSeconds 0 -PollIntervalSeconds 0 -SkipWatch `
        -QueryCommand { [pscustomobject]@{ HttpStatus = 502; Status = ""; Conclusion = "" } } `
        -SleepCommand $sleep -UtcNowCommand $utc | Out-Null
    Add-Fail "polling timeout must fail safely"
} catch {
    if ("$($_.Exception.Message)" -notmatch 'timeout') {
        Add-Fail "polling timeout must mention timeout, got: $($_.Exception.Message)"
    }
}

try {
    Wait-SpaceLensChildWorkflow -RunId "latest" -Repo "tungcorn/spacelens" -SkipWatch `
        -QueryCommand { throw "must not query an arbitrary run" } | Out-Null
    Add-Fail "non-numeric run id must be rejected"
} catch {
    if ("$($_.Exception.Message)" -notmatch 'numeric') {
        Add-Fail "non-numeric run id must be rejected as numeric, got: $($_.Exception.Message)"
    }
}

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
    $live = @(Get-SpaceLensCommitsSinceTag -Tag $repoLatest.Tag -Head "HEAD" -RepoRoot $root)
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
    if ("docs/QT_SOURCE_OFFER.md" -notin $mech) {
        Add-Fail "mechanical version files must include docs/QT_SOURCE_OFFER.md"
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
    if ("docs/QT_SOURCE_OFFER.md" -notin $changed) {
        Add-Fail "preparing $($to.Text) omitted docs/QT_SOURCE_OFFER.md"
    }
    $preparedOffer = Get-Content -LiteralPath (Join-Path $prepRoot "docs\QT_SOURCE_OFFER.md") -Raw
    $preparedCurrentZip = "spacelens-$($to.Tag)-windows-x64.zip"
    $preparedPreviousZip = "spacelens-$($from.Tag)-windows-x64.zip"
    if ($preparedOffer -notmatch [regex]::Escape("shipped with SpaceLens $($to.Text)")) {
        Add-Fail "prepared QT_SOURCE_OFFER.md must name current SpaceLens $($to.Text)"
    }
    if ($preparedOffer -match [regex]::Escape("shipped with SpaceLens $($from.Text)")) {
        Add-Fail "prepared QT_SOURCE_OFFER.md still names SpaceLens $($from.Text) as current"
    }
    if ($preparedOffer -notmatch [regex]::Escape($preparedCurrentZip)) {
        Add-Fail "verify-package would reject prepared offer: missing $preparedCurrentZip"
    }
    if ($preparedOffer -notmatch "historical\s+$([regex]::Escape($from.Tag))\s+unified archive\s+``$([regex]::Escape($preparedPreviousZip))``") {
        Add-Fail "prepared QT_SOURCE_OFFER.md must keep $($from.Text) as a historical archive"
    }
    foreach ($oldZip in @(
        "spacelens-v0.1.4-windows-x64.zip",
        "spacelens-v0.1.3-windows-x64.zip",
        "spacelens-v0.1.2-windows-x64.zip",
        "spacelens-v0.1.1-windows-x64.zip",
        "spacelens-gui-v0.1.0-windows-x64.zip"
    )) {
        if ($oldZip -eq $preparedCurrentZip) { continue }
        if ($preparedOffer -notmatch [regex]::Escape($oldZip)) {
            Add-Fail "prepared QT_SOURCE_OFFER.md lost historical archive $oldZip"
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

# Qt source offer: current archive advances; previous current becomes historical.
$offer014 = @"
This is a maintainer-controlled written offer for the Qt libraries
dynamically shipped with SpaceLens 0.1.4. It is not legal advice.

Source for the Qt 6.8.3 modules actually distributed with
``spacelens-v0.1.4-windows-x64.zip``, for as long as SpaceLens continues
to distribute those binaries. The same offer remains in force for the historical
v0.1.3 unified archive ``spacelens-v0.1.3-windows-x64.zip``, the
historical v0.1.2 unified archive ``spacelens-v0.1.2-windows-x64.zip``, the
historical v0.1.1 unified archive ``spacelens-v0.1.1-windows-x64.zip``,
and the historical v0.1.0 GUI-only archive
``spacelens-gui-v0.1.0-windows-x64.zip``.

- SHA-256: ``cdd3a69967208276bb01af7ace7dba0ba53e679f886a4cbe624225c60fb73f2c``
"@
$offer015 = Update-SpaceLensQtSourceOfferText -Text $offer014 -FromVersion "0.1.4" -ToVersion "0.1.5"
if ($offer015 -notmatch [regex]::Escape("shipped with SpaceLens 0.1.5")) {
    Add-Fail "0.1.4 -> 0.1.5 offer must name current SpaceLens 0.1.5"
}
if ($offer015 -match [regex]::Escape("shipped with SpaceLens 0.1.4")) {
    Add-Fail "0.1.4 -> 0.1.5 offer must not keep SpaceLens 0.1.4 as current"
}
if ($offer015 -notmatch [regex]::Escape("spacelens-v0.1.5-windows-x64.zip")) {
    Add-Fail "verify-package would reject 0.1.5 offer: missing spacelens-v0.1.5-windows-x64.zip"
}
if ($offer015 -notmatch "historical\s+v0\.1\.4\s+unified archive\s+``spacelens-v0\.1\.4-windows-x64\.zip``") {
    Add-Fail "0.1.4 -> 0.1.5 offer must keep v0.1.4 as a historical archive"
}
foreach ($oldZip in @(
    "spacelens-v0.1.3-windows-x64.zip",
    "spacelens-v0.1.2-windows-x64.zip",
    "spacelens-v0.1.1-windows-x64.zip",
    "spacelens-gui-v0.1.0-windows-x64.zip"
)) {
    if ($offer015 -notmatch [regex]::Escape($oldZip)) {
        Add-Fail "0.1.4 -> 0.1.5 offer lost historical archive $oldZip"
    }
}
if (($offer015 | Select-String -Pattern "spacelens-v0.1.5-windows-x64.zip" -AllMatches).Matches.Count -ne 1) {
    Add-Fail "0.1.5 current archive must appear exactly once"
}
if (($offer015 | Select-String -Pattern "spacelens-v0.1.4-windows-x64.zip" -AllMatches).Matches.Count -ne 1) {
    Add-Fail "0.1.4 historical archive must appear exactly once"
}
if ($offer015 -notmatch "cdd3a69967208276bb01af7ace7dba0ba53e679f886a4cbe624225c60fb73f2c") {
    Add-Fail "offer rewrite must keep the Qt source SHA-256 pin"
}

$offer016 = Update-SpaceLensQtSourceOfferText -Text $offer015 -FromVersion "0.1.5" -ToVersion "0.1.6"
if ($offer016 -notmatch [regex]::Escape("spacelens-v0.1.6-windows-x64.zip")) {
    Add-Fail "verify-package would reject 0.1.6 offer: missing spacelens-v0.1.6-windows-x64.zip"
}
if ($offer016 -notmatch "historical\s+v0\.1\.5\s+unified archive\s+``spacelens-v0\.1\.5-windows-x64\.zip``") {
    Add-Fail "0.1.5 -> 0.1.6 offer must keep v0.1.5 as a historical archive"
}
if ($offer016 -notmatch "historical\s+v0\.1\.4\s+unified archive\s+``spacelens-v0\.1\.4-windows-x64\.zip``") {
    Add-Fail "0.1.5 -> 0.1.6 offer must still cover historical v0.1.4"
}
if ($offer016 -match [regex]::Escape("shipped with SpaceLens 0.1.5")) {
    Add-Fail "0.1.5 -> 0.1.6 offer must not keep SpaceLens 0.1.5 as current"
}

try {
    Update-SpaceLensQtSourceOfferText -Text $offer014 -FromVersion "0.1.5" -ToVersion "0.1.6" | Out-Null
    Add-Fail "offer rewrite must refuse when the current version is not present"
} catch { }

$blind = $offer014.Replace("0.1.4", "0.1.5")
if ($blind -match [regex]::Escape("spacelens-v0.1.4-windows-x64.zip")) {
    Add-Fail "fixture sanity: a global replace should have erased the 0.1.4 zip"
}
if ($offer015 -notmatch [regex]::Escape("spacelens-v0.1.4-windows-x64.zip")) {
    Add-Fail "offer rewrite must not be a global version replace"
}

$dryDecision = [pscustomobject]@{
    Needed          = $true
    LatestVersion   = (ConvertTo-SpaceLensVersion "0.1.4")
    NextVersion     = (ConvertTo-SpaceLensVersion "0.1.5")
    ReleasableCount = 1
    Releasable      = @([pscustomobject]@{ Hash = "abc1234dead"; Subject = "feat: x" })
}
$dryText = Format-SpaceLensDryRun -Decision $dryDecision
if ($dryText -notmatch 'docs/QT_SOURCE_OFFER.md') {
    Add-Fail "prepare dry-run must list docs/QT_SOURCE_OFFER.md so a bump cannot omit it"
}

$omitRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-offer-omit-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $omitRoot | Out-Null
try {
    foreach ($rel in @(Get-SpaceLensMechanicalVersionFiles | Where-Object { $_ -ne "docs/QT_SOURCE_OFFER.md" })) {
        $dest = Join-Path $omitRoot ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
        $dir = Split-Path -Parent $dest
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
        Copy-Item -LiteralPath (Join-Path $root ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)) -Destination $dest
    }
    $from = Get-SpaceLensCMakeVersion -RepoRoot $omitRoot
    $to = Get-SpaceLensNextPatchVersion $from
    try {
        Update-SpaceLensMechanicalVersions -RepoRoot $omitRoot -FromVersion $from.Text -ToVersion $to.Text | Out-Null
        Add-Fail "prepare must fail if docs/QT_SOURCE_OFFER.md is missing"
    } catch {
        if ("$($_.Exception.Message)" -notmatch 'QT_SOURCE_OFFER') {
            Add-Fail "missing offer must fail specifically on QT_SOURCE_OFFER.md, got: $($_.Exception.Message)"
        }
    }
} finally {
    Remove-Item -LiteralPath $omitRoot -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host "verify-release-policy: $($failures.Count) failure(s)"
    exit 1
}
Write-Host "verify-release-policy: PASS"
exit 0
