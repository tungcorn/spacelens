# Validate a staged npm package and its packed tarball.
# Uses an isolated npm prefix. Does not touch the maintainer global prefix.
# Uninstall must not delete %LOCALAPPDATA%\SpaceLens.

[CmdletBinding()]
param(
    [string]$StageDir = "",
    [string]$Tarball = "",
    [switch]$SkipGui,
    [switch]$SkipInstall
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $StageDir) { $StageDir = Join-Path $root "build\npm-package" }

$failures = New-Object System.Collections.Generic.List[string]
function Add-Fail([string]$Message) {
    $script:failures.Add($Message)
    Write-Host "FAIL: $Message"
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { Add-Fail $Message }
}

function Get-AppDataStamp([string]$Path) {
    if (-not (Test-Path $Path)) { return $null }
    return @(
        Get-ChildItem $Path -Recurse -Force -File -ErrorAction SilentlyContinue |
            ForEach-Object {
                $rel = $_.FullName.Substring($Path.Length).TrimStart('\', '/')
                "{0}|{1}" -f $rel, $_.Length
            } |
            Sort-Object
    )
}

function Write-AppDataStampDiff([object]$Before, [object]$After) {
    $beforeSet = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($row in @($Before)) {
        if ($row) { [void]$beforeSet.Add([string]$row) }
    }
    $afterSet = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($row in @($After)) {
        if ($row) { [void]$afterSet.Add([string]$row) }
    }
    foreach ($row in $afterSet) {
        if (-not $beforeSet.Contains($row)) { Write-Host "  AppData + $row" }
    }
    foreach ($row in $beforeSet) {
        if (-not $afterSet.Contains($row)) { Write-Host "  AppData - $row" }
    }
}

if (-not (Test-Path $StageDir)) {
    Write-Error "staged npm package not found: $StageDir"
}

$pkg = Get-Content (Join-Path $StageDir "package.json") -Raw | ConvertFrom-Json
if ($pkg.name -ne "@tungcorn/spacelens") { Add-Fail "package name is '$($pkg.name)'" }
$cmakeText = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
    $cmakeVersion = $null
    if ($cmakeText -match 'project\(\s*SpaceLens\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        $cmakeVersion = $Matches[1]
    }
    if (-not $cmakeVersion) { Add-Fail "CMake project VERSION not found" }
    if ($cmakeVersion -and $pkg.version -ne $cmakeVersion) {
        Add-Fail "package version is '$($pkg.version)' (CMake $cmakeVersion)"
    }
if ($pkg.license -ne "MIT") { Add-Fail "package license is '$($pkg.license)'" }
if ($pkg.scripts -and $pkg.scripts.postinstall) { Add-Fail "postinstall is forbidden" }
if ($pkg.dependencies) { Add-Fail "production dependencies are forbidden" }
if ($pkg.os -notcontains "win32") { Add-Fail "os must include win32" }
if ($pkg.cpu -notcontains "x64") { Add-Fail "cpu must include x64" }

$launchers = @(
    (Join-Path $StageDir "bin\spacelens.js"),
    (Join-Path $StageDir "bin\spacelens-gui.js"),
    (Join-Path $StageDir "bin\spacelens-mcp.js"),
    (Join-Path $StageDir "bin\launch.js")
)
foreach ($launcher in $launchers) {
    if (-not (Test-Path $launcher)) {
        Add-Fail "missing launcher $launcher"
        continue
    }
    $text = Get-Content $launcher -Raw
    if ((Split-Path $launcher -Leaf) -eq "launch.js" -and $text -notmatch "shell:\s*false") {
        Add-Fail "$launcher must spawn with shell: false"
    }
    if ($text -match "shell:\s*true") {
        Add-Fail "$launcher must not use shell: true"
    }
    if ($text -match "Invoke-WebRequest|child_process\.exec(?:Sync)?\(|curl |wget ") {
        Add-Fail "$launcher looks like a downloader or shell exec"
    }
}

$native = Join-Path $StageDir "native"
& (Join-Path $root "scripts\verify-package.ps1") -MainStage $native
if ($LASTEXITCODE -ne 0) {
    Add-Fail "native payload failed verify-package.ps1"
}

Push-Location $StageDir
$dry = ""
try {
    $dry = npm pack --dry-run --ignore-scripts --json 2>$null
    if ($LASTEXITCODE -ne 0) {
        Add-Fail "npm pack --dry-run failed"
    }
} finally {
    Pop-Location
}

if ($dry) {
    try {
        $report = $dry | ConvertFrom-Json
        $entry = if ($report -is [array]) { $report[0] } else { $report }
        $names = @($entry.files | ForEach-Object { $_.path -replace '\\', '/' })
        Write-Host "dry-run files: $($names.Count); packed $($entry.filename); unpacked $($entry.unpackedSize)"
        $required = @(
            "package.json",
            "README.md",
            "LICENSE",
            "bin/spacelens.js",
            "bin/spacelens-gui.js",
            "bin/spacelens-mcp.js",
            "bin/launch.js",
            "native/spacelens.exe",
            "native/spacelens-gui.exe",
            "native/spacelens-mcp.exe",
            "native/platforms/qwindows.dll",
            "native/Qt6Core.dll",
            "native/Qt6Gui.dll",
            "native/Qt6Widgets.dll",
            "licenses/LGPL-3.0.txt",
            "licenses/GPL-3.0.txt",
            "licenses/QT_SOURCE_IDENTITY.txt",
            "licenses/QT_SOURCE_OFFER.md"
        )
        foreach ($rel in $required) {
            if ($names -notcontains $rel) {
                Add-Fail "tarball missing $rel"
            }
        }
        foreach ($name in $names) {
            $leaf = Split-Path $name -Leaf
            if ($leaf -match '\.(pdb|lib|exp|ilk|obj|prl)$') {
                Add-Fail "tarball contains developer file $name"
            }
            if ($leaf -eq "state.db" -or $leaf -eq "hash-cache.db" -or $leaf -eq "CMakeCache.txt" -or $leaf -eq ".npmrc") {
                Add-Fail "tarball contains forbidden file $name"
            }
            if ($name -match '(^|/)(\.git|tests|src|build)(/|$)') {
                Add-Fail "tarball contains forbidden path $name"
            }
        }
    } catch {
        Add-Fail "npm pack --dry-run JSON parse failed: $_"
    }
}

if (-not $Tarball) {
    $Tarball = Get-ChildItem (Join-Path $root "dist") -Filter "tungcorn-spacelens-$($pkg.version).tgz" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Tarball -or -not (Test-Path $Tarball)) {
    Add-Fail "packed tarball not found"
    $SkipInstall = $true
} else {
    Write-Host "tarball: $Tarball  size=$((Get-Item $Tarball).Length)"
}

$unit = Join-Path $root "tests\npm\test_launchers.js"
if (Test-Path $unit) {
    node $unit
    if ($LASTEXITCODE -ne 0) {
        Add-Fail "launcher unit tests failed"
    }
} else {
    Add-Fail "missing $unit"
}

if (-not $SkipInstall) {
    $prefix = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-npm-prefix-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $prefix | Out-Null
    $appData = Join-Path $env:LOCALAPPDATA "SpaceLens"
    $appDataExisted = Test-Path $appData
    $appDataStamp = Get-AppDataStamp $appData
    try {
        npm install -g --prefix $prefix --ignore-scripts $Tarball
        if ($LASTEXITCODE -ne 0) {
            Add-Fail "isolated npm install failed"
        } else {
            if ((Test-Path $appData) -ne $appDataExisted) {
                Add-Fail "npm install changed %LOCALAPPDATA%\SpaceLens existence"
            } elseif ($appDataExisted) {
                $installStamp = Get-AppDataStamp $appData
                if (($appDataStamp -join "`n") -ne ($installStamp -join "`n")) {
                    Write-AppDataStampDiff $appDataStamp $installStamp
                    Add-Fail "npm install changed %LOCALAPPDATA%\SpaceLens contents"
                }
            }
            $shim = Join-Path $prefix "spacelens.cmd"
            $guiShim = Join-Path $prefix "spacelens-gui.cmd"
            $mcpShim = Join-Path $prefix "spacelens-mcp.cmd"
            $installed = Join-Path $prefix "node_modules\@tungcorn\spacelens"
            $installedCli = Join-Path $installed "native\spacelens.exe"
            $installedMcp = Join-Path $installed "native\spacelens-mcp.exe"
            if (-not (Test-Path $shim)) { Add-Fail "missing isolated spacelens shim" }
            if (-not (Test-Path $guiShim)) { Add-Fail "missing isolated spacelens-gui shim" }
            if (-not (Test-Path $mcpShim)) { Add-Fail "missing isolated spacelens-mcp shim" }
            if (-not (Test-Path $installedCli)) { Add-Fail "missing installed native CLI" }
            if (-not (Test-Path $installedMcp)) { Add-Fail "missing installed native MCP" }

            if ((Test-Path $shim) -and (Test-Path $installedCli)) {
                $verNative = & $installedCli version 2>&1 | Out-String
                $codeNative = $LASTEXITCODE
                $verShim = & $shim version 2>&1 | Out-String
                $codeShim = $LASTEXITCODE
                Assert-True ($codeNative -eq 0) "native version exit $codeNative"
                Assert-True ($codeShim -eq $codeNative) "shim version exit $codeShim != native $codeNative"
                Assert-True ($verShim.Trim() -eq $verNative.Trim()) "shim version stdout differs from native"
                Assert-True ($verShim -match [regex]::Escape($pkg.version)) "version output missing $($pkg.version)"

                $capsNative = & $installedCli capabilities --json 2>&1 | Out-String
                $capsCodeNative = $LASTEXITCODE
                $capsShim = & $shim capabilities --json 2>&1 | Out-String
                $capsCodeShim = $LASTEXITCODE
                Assert-True ($capsCodeNative -eq 0) "native capabilities exit $capsCodeNative"
                Assert-True ($capsCodeShim -eq $capsCodeNative) "shim capabilities exit differs"
                Assert-True ($capsShim.Trim() -eq $capsNative.Trim()) "capabilities JSON was rewritten"
                try {
                    $caps = $capsShim | ConvertFrom-Json
                    Assert-True ($caps.filesystem_mutation -eq $false) "filesystem_mutation is not false"
                    Assert-True ($caps.read_only -eq $true) "read_only is not true"
                } catch {
                    Add-Fail "capabilities --json is not JSON: $capsShim"
                }

                $helpShim = & $shim help 2>&1 | Out-String
                Assert-True ($LASTEXITCODE -eq 0) "help exited $LASTEXITCODE"
                Assert-True ($helpShim -match "scan") "help missing scan"

                $badOut = & $shim not-a-real-command 2>&1 | Out-String
                $badCode = $LASTEXITCODE
                Assert-True ($badCode -ne 0) "invalid command exited 0"
                Assert-True ($badOut -match "(?i)unknown") "invalid command output unexpected"

                & (Join-Path $root "scripts\verify-cli-safety.ps1") -CliPath $installedCli
                if ($LASTEXITCODE -ne 0) {
                    Add-Fail "packaged CLI safety gate failed"
                }
                if (Test-Path $installedMcp) {
                    & (Join-Path $root "scripts\verify-mcp-safety.ps1") -McpPath $installedMcp -ExpectedVersion $pkg.version
                    if ($LASTEXITCODE -ne 0) {
                        Add-Fail "packaged native MCP safety gate failed"
                    }
                }
                if ((Test-Path $mcpShim) -and (Test-Path $installedMcp)) {
                    & (Join-Path $root "scripts\verify-mcp-wire.ps1") -McpPath $mcpShim
                    if ($LASTEXITCODE -ne 0) {
                        Add-Fail "npm spacelens-mcp shim failed the MCP wire gate"
                    }
                }
                foreach ($verb in @(@("maintenance"), @("keep-one"))) {
                    $probe = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-npm-probe-" + [guid]::NewGuid().ToString("N"))
                    New-Item -ItemType Directory -Path $probe | Out-Null
                    try {
                        $output = & $shim @verb $probe 2>&1 | Out-String
                        if ($LASTEXITCODE -eq 0) {
                            Add-Fail "destructive verb accepted: $($verb -join ' ')"
                        }
                        if ($output -match '(?i)recycled|deleted|moved|restored') {
                            Add-Fail "destructive verb looked like mutation: $($verb -join ' ')"
                        }
                    } finally {
                        Remove-Item -Recurse -Force $probe -ErrorAction SilentlyContinue
                    }
                }

                if (-not $SkipGui) {
                    $guiExe = Join-Path $installed "native\spacelens-gui.exe"
                    $qwindows = Join-Path $installed "native\platforms\qwindows.dll"
                    Assert-True (Test-Path $qwindows) "installed package missing platforms\qwindows.dll"
                    $proc = $null
                    try {
                        $proc = Start-Process -FilePath $guiShim -WorkingDirectory $prefix -PassThru -WindowStyle Minimized
                        Start-Sleep -Seconds 5
                        $guiLive = Get-Process -Name "spacelens-gui" -ErrorAction SilentlyContinue
                        if (-not $guiLive) {
                            if ($proc.HasExited) {
                                Add-Fail "spacelens-gui shim exited immediately (code $($proc.ExitCode)); Qt runtime may be incomplete"
                            } else {
                                Add-Fail "spacelens-gui.exe did not start from the npm shim"
                            }
                        } else {
                            Write-Host "GUI started via npm shim (pid $($guiLive.Id)); qwindows resolved"
                        }
                    } catch {
                        Add-Fail "failed to start spacelens-gui shim: $_"
                    } finally {
                        Get-Process -Name "spacelens-gui" -ErrorAction SilentlyContinue |
                            Stop-Process -Force -ErrorAction SilentlyContinue
                        if ($proc -and -not $proc.HasExited) {
                            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                            try { $proc.WaitForExit(5000) | Out-Null } catch { }
                        }
                    }
                }
            }

            $afterUseExisted = Test-Path $appData
            $afterUseStamp = Get-AppDataStamp $appData

            npm uninstall -g --prefix $prefix --ignore-scripts $pkg.name
            if ($LASTEXITCODE -ne 0) {
                Add-Fail "isolated npm uninstall failed"
            }
            if (Test-Path $shim) { Add-Fail "spacelens shim still present after uninstall" }
            if (Test-Path $guiShim) { Add-Fail "spacelens-gui shim still present after uninstall" }
            if (Test-Path $mcpShim) { Add-Fail "spacelens-mcp shim still present after uninstall" }
            if (Test-Path $installed) { Add-Fail "package files still present after uninstall" }

            $appDataAfter = Test-Path $appData
            if ($afterUseExisted -and -not $appDataAfter) {
                Add-Fail "npm uninstall deleted %LOCALAPPDATA%\SpaceLens"
            }
            if ($afterUseExisted) {
                $unStamp = Get-AppDataStamp $appData
                if (($afterUseStamp -join "`n") -ne ($unStamp -join "`n")) {
                    Write-AppDataStampDiff $afterUseStamp $unStamp
                    Add-Fail "npm uninstall changed %LOCALAPPDATA%\SpaceLens contents"
                }
            } elseif ($appDataAfter) {
                Add-Fail "npm uninstall created %LOCALAPPDATA%\SpaceLens"
            }
        }
    } finally {
        Remove-Item -Recurse -Force $prefix -ErrorAction SilentlyContinue
    }
}

if ($failures.Count -gt 0) {
    Write-Error "npm package validation failed ($($failures.Count) problem(s))"
}

Write-Host "npm package validation PASS"
exit 0
