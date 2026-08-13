# Import an x64 MSVC developer environment into the current PowerShell session.
# Prefers vcvars64.bat (GitHub runners + registered VS). Falls back to the
# manual INCLUDE/LIB layout used on machines where vswhere is empty.
#
# Dot-source this file, then call Import-SpaceLensMsvc.

function Find-VcVars64 {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($install) {
            $fromWhere = Join-Path $install "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $fromWhere) { return $fromWhere }
        }
    }

    $candidates = @(
        "D:\visual-studio-2026\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

function Import-CmdEnvironment([string]$BatchFile) {
    $cmd = "`"$BatchFile`" >nul && set"
    $output = & cmd.exe /c $cmd
    foreach ($line in $output) {
        $eq = $line.IndexOf("=")
        if ($eq -lt 1) { continue }
        $name = $line.Substring(0, $eq)
        $value = $line.Substring($eq + 1)
        Set-Item -Path "Env:$name" -Value $value
    }
}

function Import-SpaceLensMsvc {
    $vcvars = Find-VcVars64
    if ($vcvars) {
        Import-CmdEnvironment $vcvars
        Write-Host "MSVC environment imported from $vcvars"
        $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
        if (-not $cl) {
            Write-Error "vcvars64 ran but cl.exe is not on PATH."
        }
        Write-Host "  cl: $($cl.Source)"
        return $true
    }

    Write-Warning "vcvars64.bat not found; falling back to scripts/dev-env.ps1 layout."
    return $false
}
