# Install a pinned Qt MSVC x64 kit with aqtinstall.
# Used by CI. Local developers may use an existing Qt prefix instead.
#
# Pins:
#   Qt        6.8.3 win64_msvc2022_64
#   aqtinstall 3.3.0
#
# Success stdout is exactly one line: the Qt prefix. Everything else is
# Write-Host so `$prefix = & .\scripts\install-qt.ps1` is a real path.

[CmdletBinding()]
param(
    [string]$Version = "6.8.3",
    [string]$Arch = "win64_msvc2022_64",
    [string]$AqtVersion = "3.3.0",
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"

function Invoke-LoggedNative {
    param([Parameter(Mandatory = $true)][string[]]$NativeArgs)
    $oldEap = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
            $PSNativeCommandUseErrorActionPreference = $false
        }
        $output = & $NativeArgs[0] @($NativeArgs[1..($NativeArgs.Length - 1)]) 2>&1
        $code = $LASTEXITCODE
        foreach ($line in $output) { Write-Host $line }
        if ($code -ne 0) {
            throw "Command failed with exit $code : $($NativeArgs -join ' ')"
        }
    } finally {
        $ErrorActionPreference = $oldEap
    }
}

if (-not $Output) {
    if ($env:RUNNER_TEMP) {
        $Output = Join-Path $env:RUNNER_TEMP "Qt"
    } else {
        $Output = Join-Path ([System.IO.Path]::GetTempPath()) "spacelens-qt"
    }
}

$prefix = Join-Path $Output "$Version\msvc2022_64"
$config = Join-Path $prefix "lib\cmake\Qt6\Qt6Config.cmake"
if (Test-Path $config) {
    Write-Host "Qt $Version already present at $prefix"
    $env:CMAKE_PREFIX_PATH = $prefix
    Write-Host "CMAKE_PREFIX_PATH=$prefix"
    Write-Output $prefix
    return
}

Invoke-LoggedNative @(
    "python", "-m", "pip", "install", "--disable-pip-version-check", "aqtinstall==$AqtVersion"
)

function Get-Verified7Zip {
    $candidates = New-Object System.Collections.Generic.List[string]
    $cmd = Get-Command "7z" -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) {
        $candidates.Add($cmd.Source)
    }
    if ($env:ProgramFiles) {
        $candidates.Add((Join-Path $env:ProgramFiles "7-Zip\7z.exe"))
    }
    $pf86 = Get-ChildItem Env: | Where-Object { $_.Name -eq "ProgramFiles(x86)" } | Select-Object -ExpandProperty Value -ErrorAction SilentlyContinue
    if ($pf86) {
        $candidates.Add((Join-Path $pf86 "7-Zip\7z.exe"))
    }
    foreach ($exe in $candidates) {
        if (-not (Test-Path -LiteralPath $exe)) { continue }
        try {
            $null = & $exe i 2>&1
            if ($LASTEXITCODE -eq 0) {
                Write-Host "Verified external 7-Zip at $exe"
                return $exe
            }
        } catch {
            # Verification failed
        }
    }
    Write-Host "External 7-Zip not found or verification failed; using default py7zr extraction"
    return $null
}

$sevenZip = Get-Verified7Zip
$maxAttempts = 2
$success = $false

for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
    Write-Host "Qt install attempt $attempt/$maxAttempts"

    if (Test-Path -LiteralPath $Output) {
        Remove-Item -LiteralPath $Output -Recurse -Force -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Force -Path $Output | Out-Null

    $aqtCmd = @(
        "python", "-m", "aqt", "install-qt", "windows", "desktop", $Version, $Arch, "-O", $Output
    )
    if ($sevenZip) {
        $aqtCmd += @("--external", $sevenZip)
    }

    try {
        Invoke-LoggedNative $aqtCmd
        if (Test-Path -LiteralPath $config) {
            $success = $true
            break
        } else {
            Write-Host "Qt6Config.cmake not found at $config after attempt $attempt"
        }
    } catch {
        Write-Host "Qt install attempt $attempt failed: $_"
    }

    if ($attempt -lt $maxAttempts) {
        Write-Host "retrying after install failure"
    }
}

if (-not $success -or -not (Test-Path -LiteralPath $config)) {
    throw "Qt $Version installation failed after $maxAttempts attempts"
}

$env:CMAKE_PREFIX_PATH = $prefix
Write-Host "CMAKE_PREFIX_PATH=$prefix"
Write-Output $prefix

