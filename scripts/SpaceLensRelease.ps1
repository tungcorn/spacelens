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

function Get-SpaceLensLsRemoteSha {
    # Parse `git ls-remote` output. Missing refs produce no lines; never index
    # an empty split (StrictMode throws "Index was outside the bounds of the array").
    param(
        [AllowNull()]
        [AllowEmptyString()]
        [AllowEmptyCollection()]
        $Output = $null
    )
    foreach ($line in @($Output)) {
        if ($null -eq $line) { continue }
        $text = "$line".Trim()
        if (-not $text) { continue }
        $parts = @($text -split '\s+' | Where-Object { $_ })
        if ($parts.Count -gt 0) {
            return [string]$parts[0]
        }
    }
    return ""
}

function Get-SpaceLensRemoteTagSha {
    param(
        [Parameter(Mandatory = $true)][string]$Tag,
        [string]$Remote = "origin"
    )
    $ref = "$Tag".Trim()
    if (-not $ref) { return "" }
    if ($ref -notmatch '^refs/tags/') {
        $ref = "refs/tags/$ref"
    }
    if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
        $PSNativeCommandUseErrorActionPreference = $false
    }
    $output = & git ls-remote $Remote $ref 2>$null
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "git ls-remote $Remote $ref failed with exit $code"
    }
    return (Get-SpaceLensLsRemoteSha -Output $output)
}

function Get-SpaceLensPrepareIdempotency {
    param(
        [string]$MainCMakeVersion = "",
        [Parameter(Mandatory = $true)][string]$NextVersion,
        [string]$TagSha = "",
        [string]$MainSha = "",
        [string]$MainSubject = ""
    )
    $alreadyBumped = ($MainCMakeVersion -eq $NextVersion)
    $tagExists = -not [string]::IsNullOrWhiteSpace($TagSha)
    $bumpSha = ""
    if ($alreadyBumped -and -not $tagExists) {
        if (Test-SpaceLensReleasePrepSubject $MainSubject -ExpectedVersion $NextVersion) {
            $bumpSha = "$MainSha".Trim()
        }
    }
    return [pscustomobject]@{
        AlreadyBumped   = [bool]$alreadyBumped
        TagExists       = [bool]$tagExists
        BumpSha         = $bumpSha
        ContinuePrepare = [bool]((-not $alreadyBumped) -and (-not $tagExists))
    }
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

function Test-SpaceLensDocPath {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path)
    $p = $Path.Replace("\", "/").Trim().ToLowerInvariant()
    if ([string]::IsNullOrWhiteSpace($p)) { return $false }
    if ($p -eq "readme.md" -or $p -eq "changelog.md" -or $p -eq "contributing.md" -or $p -eq "license") {
        return $true
    }
    if ($p.StartsWith("docs/")) {
        return $true
    }
    if ($p.EndsWith(".md")) {
        if ($p.StartsWith("src/") -or $p.StartsWith("tests/") -or $p.StartsWith("scripts/") -or $p.StartsWith("packaging/") -or $p.StartsWith(".github/") -or $p.StartsWith("cmake/") -or $p.StartsWith("third_party/")) {
            return $false
        }
        return $true
    }
    return $false
}

function Test-SpaceLensPinOnlyPath {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path)
    $p = $Path.Replace("\", "/").Trim().ToLowerInvariant()
    return $p -eq "packaging/npm/release-pin.env"
}

function Get-SpaceLensCiChangeClass {
    param([AllowEmptyCollection()][string[]]$Paths = @())
    $clean = @($Paths | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
    if ($clean.Count -eq 0) {
        return [pscustomobject]@{
            DocsOnly      = $false
            PinOnly       = $false
            RunExpensive  = $true
        }
    }
    $hasPin = $false
    $hasOther = $false
    foreach ($path in $clean) {
        if (Test-SpaceLensPinOnlyPath $path) {
            $hasPin = $true
            continue
        }
        if (-not (Test-SpaceLensDocPath $path)) {
            $hasOther = $true
        }
    }
    if ($hasOther) {
        return [pscustomobject]@{
            DocsOnly     = $false
            PinOnly      = $false
            RunExpensive = $true
        }
    }
    if ($hasPin) {
        return [pscustomobject]@{
            DocsOnly     = $false
            PinOnly      = $true
            RunExpensive = $false
        }
    }
    return [pscustomobject]@{
        DocsOnly     = $true
        PinOnly      = $false
        RunExpensive = $false
    }
}

function Get-SpaceLensPrepareNeed {
    param(
        [bool]$RunPipeline = $false,
        [string]$EventName = "",
        [bool]$ReleaseNeeded = $false,
        [string]$WorkflowRunConclusion = "success",
        [string]$WorkflowRunBranch = "main",
        [string]$WorkflowRunEvent = "push",
        [string]$HeadSha = "head"
    )
    if ($EventName -eq "workflow_dispatch") { return $false }
    if ($RunPipeline) { return $false }
    if ($EventName -eq "workflow_run") {
        if ($WorkflowRunConclusion -ne "success" -or
            $WorkflowRunBranch -ne "main" -or
            $WorkflowRunEvent -ne "push" -or
            [string]::IsNullOrWhiteSpace($HeadSha)) {
            return $false
        }
    }
    return [bool]$ReleaseNeeded
}

function Get-SpaceLensEnsureCiDecision {
    param([AllowEmptyCollection()][object[]]$Runs = @())
    foreach ($run in @($Runs)) {
        if ([string]$run.conclusion -eq "success") { return "reuse" }
    }
    foreach ($run in @($Runs)) {
        $status = [string]$run.status
        if ($status -in @("in_progress", "queued", "waiting", "pending", "requested")) {
            return "reuse"
        }
    }
    return "dispatch"
}

function Get-SpaceLensGitHubApiObservation {
    param(
        [AllowNull()]
        [AllowEmptyString()]
        $Output = "",
        [int]$ExitCode = 0
    )
    $text = if ($null -eq $Output) { "" } else { "$Output" }
    $http = 0
    $status = ""
    $conclusion = ""
    if ($ExitCode -eq 0) {
        # Successful gh api: only JSON status/conclusion count. Commit
        # messages may mention "HTTP 404" and must not be treated as the
        # transport status.
        $http = 200
        if ($text.Trim()) {
            try {
                $json = $text | ConvertFrom-Json
                if ($json -and $json.PSObject.Properties.Name -contains 'status') {
                    $status = [string]$json.status
                }
                if ($json -and $json.PSObject.Properties.Name -contains 'conclusion') {
                    $conclusion = [string]$json.conclusion
                }
            } catch {
                $status = ""
                $conclusion = ""
            }
        }
    } elseif ($text -match '(?im)(?:^|\b)HTTP\s+(\d{3})\b') {
        $http = [int]$Matches[1]
    }
    return [pscustomobject]@{
        HttpStatus = $http
        Status     = $status
        Conclusion = $conclusion
        ExitCode   = $ExitCode
    }
}

function Get-SpaceLensWorkflowWatchVerdict {
    param(
        [int]$WatchExitCode = 0,
        [string]$Status = "",
        [string]$Conclusion = "",
        [int]$HttpStatus = 0,
        [switch]$TimedOut
    )
    if ($WatchExitCode -eq 0) {
        return [pscustomobject]@{
            Action = 'continue'
            Reason = 'gh run watch succeeded'
        }
    }
    if ($TimedOut) {
        return [pscustomobject]@{
            Action = 'fail'
            Reason = 'child workflow never reached a trustworthy completed state before timeout'
        }
    }
    if ($HttpStatus -eq 429 -or $HttpStatus -ge 500 -or $HttpStatus -eq 0) {
        return [pscustomobject]@{
            Action = 'retry'
            Reason = "transient API/observation failure HTTP $HttpStatus"
        }
    }
    if ($HttpStatus -ge 400 -and $HttpStatus -lt 500) {
        return [pscustomobject]@{
            Action = 'fail'
            Reason = "GitHub API client error HTTP $HttpStatus"
        }
    }
    $state = "$Status".Trim().ToLowerInvariant()
    if ($state -eq 'completed') {
        $done = "$Conclusion".Trim().ToLowerInvariant()
        if ($done -eq 'success') {
            return [pscustomobject]@{
                Action = 'continue'
                Reason = 'child workflow conclusion=success'
            }
        }
        if (-not $done) {
            return [pscustomobject]@{
                Action = 'fail'
                Reason = 'child workflow completed without a conclusion'
            }
        }
        return [pscustomobject]@{
            Action = 'fail'
            Reason = "child workflow conclusion=$done"
        }
    }
    if ($state -in @('queued', 'in_progress', 'waiting', 'pending', 'requested', 'waiting_for_review')) {
        return [pscustomobject]@{
            Action = 'retry'
            Reason = "child workflow still $state"
        }
    }
    if ($state) {
        return [pscustomobject]@{
            Action = 'retry'
            Reason = "child workflow status '$state' is not yet completed"
        }
    }
    return [pscustomobject]@{
        Action = 'retry'
        Reason = 'no trustworthy child workflow conclusion yet'
    }
}

function Wait-SpaceLensChildWorkflow {
    param(
        [Parameter(Mandatory = $true)][string]$RunId,
        [string]$Repo = "",
        [int]$PollTimeoutSeconds = 1800,
        [int]$PollIntervalSeconds = 15,
        [switch]$SkipWatch,
        [scriptblock]$WatchCommand,
        [scriptblock]$QueryCommand,
        [scriptblock]$SleepCommand,
        [scriptblock]$UtcNowCommand
    )
    $id = "$RunId".Trim()
    if ($id -notmatch '^\d+$') {
        throw "RunId '$RunId' is not a numeric workflow run id"
    }
    if ($PollTimeoutSeconds -lt 0) { throw "PollTimeoutSeconds must be >= 0" }
    if ($PollIntervalSeconds -lt 0) { throw "PollIntervalSeconds must be >= 0" }

    $now = {
        if ($UtcNowCommand) { & $UtcNowCommand } else { [datetime]::UtcNow }
    }
    $sleep = {
        param([int]$Seconds)
        if ($SleepCommand) { & $SleepCommand $Seconds }
        elseif ($Seconds -gt 0) { Start-Sleep -Seconds $Seconds }
    }

    if (-not $SkipWatch) {
        if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
            $PSNativeCommandUseErrorActionPreference = $false
        }
        $watchExit = 0
        if ($WatchCommand) {
            $watchExit = [int](& $WatchCommand $id $Repo)
        } else {
            if (-not $Repo) { throw "Repo is required to watch a workflow run" }
            & gh run watch $id --repo $Repo --exit-status | Out-Host
            $watchExit = $LASTEXITCODE
        }
        if ($watchExit -eq 0) {
            return [pscustomobject]@{
                Action = 'continue'
                Reason = 'gh run watch succeeded'
            }
        }
        Write-Host "gh run watch returned non-zero; verifying authoritative workflow conclusion via API..."
    }

    $deadline = (& $now).AddSeconds($PollTimeoutSeconds)
    while ($true) {
        $obs = $null
        if ($QueryCommand) {
            $obs = & $QueryCommand $id $Repo
        } else {
            if (-not $Repo) { throw "Repo is required to query a workflow run" }
            if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
                $PSNativeCommandUseErrorActionPreference = $false
            }
            $output = & gh api "repos/$Repo/actions/runs/$id" 2>&1
            $apiExit = $LASTEXITCODE
            $obs = Get-SpaceLensGitHubApiObservation -Output ($output | Out-String) -ExitCode $apiExit
        }
        if (-not $obs) {
            $obs = [pscustomobject]@{ HttpStatus = 0; Status = ""; Conclusion = "" }
        }
        $http = 0
        $status = ""
        $conclusion = ""
        if ($obs.PSObject.Properties.Name -contains 'HttpStatus') { $http = [int]$obs.HttpStatus }
        if ($obs.PSObject.Properties.Name -contains 'Status') { $status = [string]$obs.Status }
        if ($obs.PSObject.Properties.Name -contains 'Conclusion') { $conclusion = [string]$obs.Conclusion }

        $verdict = Get-SpaceLensWorkflowWatchVerdict `
            -WatchExitCode 1 `
            -Status $status `
            -Conclusion $conclusion `
            -HttpStatus $http
        Write-Host "child workflow $id status='$status' conclusion='$conclusion' http=$http action=$($verdict.Action)"
        if ($verdict.Action -eq 'continue') {
            return $verdict
        }
        if ($verdict.Action -eq 'fail') {
            throw $verdict.Reason
        }
        if ((& $now) -ge $deadline) {
            $timeout = Get-SpaceLensWorkflowWatchVerdict -WatchExitCode 1 -TimedOut
            throw $timeout.Reason
        }
        & $sleep $PollIntervalSeconds
    }
}

function Get-SpaceLensAutomationIntent {
    param(
        [Parameter(Mandatory = $true)][string]$EventName,
        [string]$Subject = "",
        [string[]]$Paths = @(),
        [Parameter(Mandatory = $true)][string]$CMakeVersion,
        [Parameter(Mandatory = $true)][string]$LatestStableVersion,
        [string]$ParentCMakeVersion = "",
        [string]$HeadSha = "head",
        [string]$TagPeelSha = "",
        [string]$PublishInput = "",
        [object[]]$RangeCommits = @(),
        [object[]]$ExistingCiRuns = @(),
        [string]$WorkflowRunConclusion = "success",
        [string]$WorkflowRunBranch = "main",
        [string]$WorkflowRunEvent = "push"
    )
    $ciClass = Get-SpaceLensCiChangeClass -Paths $Paths
    $commits = @($RangeCommits)
    if ($commits.Count -eq 0 -and $Subject) {
        $commits = @([pscustomobject]@{ Subject = $Subject; Paths = $Paths })
    }
    $decision = Get-SpaceLensReleaseDecision -Commits $commits -LatestVersion $LatestStableVersion
    $parent = $null
    if ($ParentCMakeVersion) { $parent = $ParentCMakeVersion }
    $auto = Test-SpaceLensAutoPublishCommit `
        -CMakeVersion $CMakeVersion `
        -LatestStableVersion $LatestStableVersion `
        -ParentCMakeVersion $parent `
        -HeadSha $HeadSha `
        -TagPeelSha $TagPeelSha

    $runPipeline = $false
    $publish = $false
    if ($EventName -eq "workflow_dispatch") {
        $runPipeline = $true
        $publish = ($PublishInput -eq "true")
    } elseif ($auto) {
        $runPipeline = $true
        $publish = $true
    }

    $prepareNeeded = Get-SpaceLensPrepareNeed `
        -RunPipeline $runPipeline `
        -EventName $EventName `
        -ReleaseNeeded ([bool]$decision.Needed) `
        -WorkflowRunConclusion $WorkflowRunConclusion `
        -WorkflowRunBranch $WorkflowRunBranch `
        -WorkflowRunEvent $WorkflowRunEvent `
        -HeadSha $HeadSha
    $ensureCi = "skip"
    if ($runPipeline -and $publish) {
        $ensureCi = Get-SpaceLensEnsureCiDecision -Runs $ExistingCiRuns
    }

    return [pscustomobject]@{
        RunPipeline        = [bool]$runPipeline
        Publish            = [bool]$publish
        PrepareNeeded      = [bool]$prepareNeeded
        ReleaseNeeded      = [bool]$decision.Needed
        NextVersion        = $decision.NextVersion
        DocsOnly           = [bool]$ciClass.DocsOnly
        PinOnly            = [bool]$ciClass.PinOnly
        RunExpensiveCi     = [bool]$ciClass.RunExpensive
        DispatchCiAfterPin = $false
        EnsureCi           = $ensureCi
        LoopExcluded       = [bool](Test-SpaceLensReleaseLoopSubject $Subject)
    }
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
    if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
        $PSNativeCommandUseErrorActionPreference = $false
    }
    $range = "$Tag..$Head"
    $raw = & git -C $RepoRoot log $range --format="__COMMIT__%n%H%n%s" --name-only
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "git log $range failed"
    }
    $commits = New-Object System.Collections.Generic.List[object]
    $hash = $null
    $subject = $null
    $paths = New-Object System.Collections.Generic.List[string]
    foreach ($line in @($raw)) {
        if ($null -eq $line) { continue }
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
    # Callers must assign with @(...). A bare empty ToArray() unwraps to no
    # output; @() turns that into a real empty collection, not $null.
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

function Update-SpaceLensQtSourceOfferText {
    # Advance the current SpaceLens version and unified archive only.
    # The previous current archive becomes historical. Existing historical
    # v0.1.3 / v0.1.2 / v0.1.1 / v0.1.0 (GUI-only) entries are preserved.
    # This is not a global version replace.
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$FromVersion,
        [Parameter(Mandatory = $true)][string]$ToVersion
    )
    $from = ConvertTo-SpaceLensVersion $FromVersion
    $to = ConvertTo-SpaceLensVersion $ToVersion
    if (-not $from -or -not $to) {
        throw "QT_SOURCE_OFFER.md versions must be X.Y.Z (from='$FromVersion' to='$ToVersion')"
    }
    if ($from.Text -eq $to.Text) {
        throw "QT_SOURCE_OFFER.md from and to versions are the same ($($from.Text))"
    }

    $fromZip = "spacelens-$($from.Tag)-windows-x64.zip"
    $toZip = "spacelens-$($to.Tag)-windows-x64.zip"
    $fromZipTick = '`' + $fromZip + '`'
    $toZipTick = '`' + $toZip + '`'
    $shippedFrom = "shipped with SpaceLens $($from.Text)"
    $shippedTo = "shipped with SpaceLens $($to.Text)"
    $ordinal = [System.StringComparison]::Ordinal

    if ($Text.IndexOf($shippedFrom, $ordinal) -lt 0) {
        throw "QT_SOURCE_OFFER.md does not name current SpaceLens $($from.Text)"
    }
    if ($Text.IndexOf($shippedTo, $ordinal) -ge 0) {
        throw "QT_SOURCE_OFFER.md already names SpaceLens $($to.Text) as current"
    }

    $distAt = $Text.IndexOf("distributed with", $ordinal)
    if ($distAt -lt 0) {
        throw "QT_SOURCE_OFFER.md is missing the current 'distributed with' archive sentence"
    }
    $histAt = $Text.IndexOf("The same offer remains in force", $distAt, $ordinal)
    if ($histAt -lt 0) {
        throw "QT_SOURCE_OFFER.md is missing the historical archive sentence"
    }

    $currentRegion = $Text.Substring($distAt, $histAt - $distAt)
    $fromZipAt = $currentRegion.IndexOf($fromZipTick, $ordinal)
    if ($fromZipAt -lt 0) {
        throw "QT_SOURCE_OFFER.md current archive is not $fromZip"
    }
    $secondFrom = $currentRegion.IndexOf($fromZipTick, $fromZipAt + $fromZipTick.Length, $ordinal)
    if ($secondFrom -ge 0) {
        throw "QT_SOURCE_OFFER.md current region names $fromZip more than once"
    }
    if ($currentRegion.IndexOf($toZipTick, $ordinal) -ge 0) {
        throw "QT_SOURCE_OFFER.md current region already names $toZip"
    }

    $historicalArchives = New-Object System.Collections.Generic.List[string]
    foreach ($m in [regex]::Matches($Text, 'historical\s+v\d+\.\d+\.\d+\s+(?:unified|GUI-only)\s+archive\s+(`[^`]+`)')) {
        $historicalArchives.Add($m.Groups[1].Value)
    }

    $newCurrent = $currentRegion.Substring(0, $fromZipAt) + $toZipTick + $currentRegion.Substring($fromZipAt + $fromZipTick.Length)
    $updated = $Text.Substring(0, $distAt) + $newCurrent + $Text.Substring($histAt)

    $shippedAt = $updated.IndexOf($shippedFrom, $ordinal)
    if ($shippedAt -lt 0) {
        throw "QT_SOURCE_OFFER.md lost current SpaceLens $($from.Text) while rewriting"
    }
    $updated = $updated.Substring(0, $shippedAt) + $shippedTo + $updated.Substring($shippedAt + $shippedFrom.Length)

    $alreadyHistorical = [regex]::IsMatch(
        $updated,
        "historical\s+$([regex]::Escape($from.Tag))\s+unified archive\s+$([regex]::Escape($fromZipTick))"
    )
    if (-not $alreadyHistorical) {
        $intro = "The same offer remains in force for the historical"
        $histInsertAt = $updated.IndexOf($intro, $ordinal)
        if ($histInsertAt -lt 0) {
            throw "QT_SOURCE_OFFER.md historical list intro is missing"
        }
        $afterIntro = $histInsertAt + $intro.Length
        $rest = $updated.Substring($afterIntro)
        if ($rest -notmatch '^(?<ws>\s+)') {
            throw "QT_SOURCE_OFFER.md historical list has no whitespace after intro"
        }
        $ws = $Matches['ws']
        $afterWs = $afterIntro + $ws.Length
        $insertion = "$($from.Tag) unified archive $fromZipTick, the${ws}historical "
        $updated = $updated.Substring(0, $afterWs) + $insertion + $updated.Substring($afterWs)
    }

    foreach ($archiveTick in $historicalArchives) {
        if ($updated.IndexOf($archiveTick, $ordinal) -lt 0) {
            throw "QT_SOURCE_OFFER.md lost historical coverage: $archiveTick"
        }
    }
    if ($updated.IndexOf($toZipTick, $ordinal) -lt 0) {
        throw "QT_SOURCE_OFFER.md did not name current archive $toZip"
    }
    if ($updated.IndexOf($fromZipTick, $ordinal) -lt 0) {
        throw "QT_SOURCE_OFFER.md dropped previous archive $fromZip"
    }
    if ($updated.IndexOf($shippedTo, $ordinal) -lt 0) {
        throw "QT_SOURCE_OFFER.md did not name current SpaceLens $($to.Text)"
    }
    if ($updated.IndexOf($shippedFrom, $ordinal) -ge 0) {
        throw "QT_SOURCE_OFFER.md still names SpaceLens $($from.Text) as current"
    }
    $qtPin = "cdd3a69967208276bb01af7ace7dba0ba53e679f886a4cbe624225c60fb73f2c"
    if ($Text.IndexOf($qtPin, $ordinal) -ge 0 -and $updated.IndexOf($qtPin, $ordinal) -lt 0) {
        throw "QT_SOURCE_OFFER.md lost the Qt source SHA-256 pin"
    }

    return $updated
}

function Get-SpaceLensMechanicalVersionFiles {
    # Product version sources plus the written Qt source offer. The offer
    # is rewritten with historical-preserving logic, not a global replace.
    # Workflows stay version-agnostic so a GITHUB_TOKEN push does not need
    # the `workflows` permission.
    return @(
        "CMakeLists.txt",
        "packaging/npm/package.json",
        "src/cli/main.cpp",
        "src/cli/Commands.cpp",
        "src/mcp/Protocol.hpp",
        "src/app/Application.cpp",
        "src/core/StorageAnalysis.cpp",
        "docs/QT_SOURCE_OFFER.md"
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
        @{ Rel = "src/mcp/Protocol.hpp"; Pattern = '(#define SPACELENS_MCP_VERSION ")' + $from + '(")'; Replace = "`${1}$ToVersion`${2}" }
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

    $offerRel = "docs/QT_SOURCE_OFFER.md"
    $offerPath = Join-Path $RepoRoot ($offerRel -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $offerPath)) {
        throw "version file missing: $offerRel"
    }
    $offerText = Get-Content -LiteralPath $offerPath -Raw
    $updatedOffer = Update-SpaceLensQtSourceOfferText -Text $offerText -FromVersion $FromVersion -ToVersion $ToVersion
    if ($updatedOffer -eq $offerText) {
        throw "did not update version in $offerRel"
    }
    $utf8Offer = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($offerPath, $updatedOffer, $utf8Offer)
    $changed.Add($offerRel)

    foreach ($rel in Get-SpaceLensMechanicalVersionFiles) {
        if ($rel -notin $changed) {
            throw "did not update version in $rel"
        }
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
