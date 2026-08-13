# Category checks for staged CLI-only and unified main portable trees.
# Does not hardcode every Qt plugin DLL. Fails on missing runtime,
# developer leftovers, CLI Qt/maintenance contamination, and
# Windows SDK D3D/DXC compilers.

[CmdletBinding()]
param(
    [string]$CliStage = "",
    [Alias("GuiStage")]
    [string]$MainStage = ""
)

$ErrorActionPreference = "Stop"

if (-not $CliStage -and -not $MainStage) {
    Write-Error "Specify -CliStage and/or -MainStage"
}

$failures = New-Object System.Collections.Generic.List[string]

function Add-Fail([string]$Message) {
    $script:failures.Add($Message)
    Write-Host "FAIL: $Message"
}

function Get-Rel([string]$Root, [System.IO.FileSystemInfo]$Item) {
    $full = $Item.FullName
    if ($full.StartsWith($Root, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($Root.Length).TrimStart("\", "/")
    }
    return $Item.Name
}

function Test-RequiredFile([string]$Root, [string]$Rel) {
    $path = Join-Path $Root $Rel
    if (-not (Test-Path $path)) {
        Add-Fail "missing required file: $Rel"
        return $null
    }
    return Get-Item $path
}

function Test-ForbiddenName([string]$Root, [string[]]$Names) {
    if (-not (Test-Path $Root)) { return }
    Get-ChildItem $Root -Recurse -Force -File -ErrorAction SilentlyContinue | ForEach-Object {
        foreach ($n in $Names) {
            if ($_.Name -like $n) {
                Add-Fail "forbidden file $(Get-Rel $Root $_)"
            }
        }
    }
}

function Test-ForbiddenDir([string]$Root, [string[]]$DirNames) {
    if (-not (Test-Path $Root)) { return }
    Get-ChildItem $Root -Recurse -Force -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        foreach ($n in $DirNames) {
            if ($_.Name -eq $n) {
                Add-Fail "forbidden directory $(Get-Rel $Root $_)"
            }
        }
    }
}

# Microsoft Windows SDK D3D redist (x64) that windeployqt copies without
# --no-system-d3d-compiler. The Qt 6.8.3 kit copy is a different size.
$sdkD3dCompilerBytes = 4741488

function Test-D3dOrigin([string]$Root) {
    $d3d = Get-ChildItem $Root -Recurse -Force -File -Filter "d3dcompiler_47.dll" -ErrorAction SilentlyContinue
    foreach ($f in $d3d) {
        if ($f.Length -eq $sdkD3dCompilerBytes) {
            Add-Fail "Windows SDK d3dcompiler_47.dll ($($f.Length) bytes) must not be shipped: $(Get-Rel $Root $f)"
        } else {
            Write-Host "note: d3dcompiler_47.dll $($f.Length) bytes (not SDK redist size $sdkD3dCompilerBytes): $(Get-Rel $Root $f)"
        }
    }
}

if ($CliStage) {
    if (-not (Test-Path $CliStage)) {
        Add-Fail "CLI stage not found: $CliStage"
    } else {
        $cliRoot = (Resolve-Path $CliStage).Path
        Write-Host "Verifying CLI stage: $cliRoot"
        Test-RequiredFile $cliRoot "spacelens.exe" | Out-Null
        Test-RequiredFile $cliRoot "README.txt" | Out-Null
        Test-RequiredFile $cliRoot "THIRD_PARTY_NOTICES.txt" | Out-Null
        $cliLicense = Test-RequiredFile $cliRoot "LICENSE"
        if ($cliLicense -and [string]::IsNullOrWhiteSpace((Get-Content $cliLicense.FullName -Raw -ErrorAction SilentlyContinue))) {
            Add-Fail "CLI LICENSE is empty"
        }

        Test-ForbiddenName $cliRoot @(
            "spacelens-gui.exe",
            "spacelens_tests.exe",
            "Qt6*.dll",
            "opengl32sw.dll",
            "d3dcompiler_47.dll",
            "dxcompiler.dll",
            "dxil.dll",
            "vcruntime*.dll",
            "msvcp*.dll",
            "msvcr*.dll",
            "concrt*.dll",
            "*.pdb",
            "*.lib",
            "*.exp",
            "*.ilk",
            "*.obj",
            "state.db",
            "CMakeCache.txt"
        )
        Test-ForbiddenDir $cliRoot @("platforms", "include", "cmake", "lib")

        $maint = Get-ChildItem $cliRoot -Recurse -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match "maintenance" }
        foreach ($m in $maint) {
            Add-Fail "CLI stage must not contain maintenance artifacts: $(Get-Rel $cliRoot $m)"
        }
    }
}

if ($MainStage) {
    if (-not (Test-Path $MainStage)) {
        Add-Fail "main stage not found: $MainStage"
    } else {
        $guiRoot = (Resolve-Path $MainStage).Path
        Write-Host "Verifying unified main stage: $guiRoot"
        Test-RequiredFile $guiRoot "spacelens.exe" | Out-Null
        Test-RequiredFile $guiRoot "spacelens-gui.exe" | Out-Null
        Test-RequiredFile $guiRoot "README.txt" | Out-Null
        Test-RequiredFile $guiRoot "THIRD_PARTY_NOTICES.txt" | Out-Null
        Test-RequiredFile $guiRoot "platforms\qwindows.dll" | Out-Null
        Test-RequiredFile $guiRoot "Qt6Core.dll" | Out-Null
        Test-RequiredFile $guiRoot "Qt6Gui.dll" | Out-Null
        Test-RequiredFile $guiRoot "Qt6Widgets.dll" | Out-Null
        Test-RequiredFile $guiRoot "LICENSE" | Out-Null
        $guiLicense = Join-Path $guiRoot "LICENSE"
        if ((Test-Path $guiLicense) -and [string]::IsNullOrWhiteSpace((Get-Content $guiLicense -Raw -ErrorAction SilentlyContinue))) {
            Add-Fail "main LICENSE is empty"
        }
        Test-RequiredFile $guiRoot "licenses\LGPL-3.0.txt" | Out-Null
        Test-RequiredFile $guiRoot "licenses\GPL-3.0.txt" | Out-Null
        Test-RequiredFile $guiRoot "licenses\QT_SOURCE_IDENTITY.txt" | Out-Null
        Test-RequiredFile $guiRoot "licenses\QT_SOURCE_OFFER.md" | Out-Null
        $identity = Join-Path $guiRoot "licenses\QT_SOURCE_IDENTITY.txt"
        if (Test-Path $identity) {
            $idText = Get-Content $identity -Raw
            if ($idText -notmatch "qt-everywhere-src-6.8.3.tar.xz") {
                Add-Fail "QT_SOURCE_IDENTITY.txt missing archive name"
            }
            if ($idText -notmatch "cdd3a69967208276bb01af7ace7dba0ba53e679f886a4cbe624225c60fb73f2c") {
                Add-Fail "QT_SOURCE_IDENTITY.txt missing official SHA-256 pin"
            }
        }

        Test-ForbiddenName $guiRoot @(
            "spacelens_tests.exe",
            "dxcompiler.dll",
            "dxil.dll",
            "vcruntime*.dll",
            "msvcp*.dll",
            "msvcr*.dll",
            "concrt*.dll",
            "*.pdb",
            "*.lib",
            "*.exp",
            "*.ilk",
            "*.obj",
            "*.prl",
            "state.db",
            "CMakeCache.txt",
            "Qt6EntryPoint*.lib"
        )
        Test-ForbiddenDir $guiRoot @("include", "cmake", "lib")
        Test-D3dOrigin $guiRoot

        $lgpl = Join-Path $guiRoot "licenses\LGPL-3.0.txt"
        if (Test-Path $lgpl) {
            $head = Get-Content $lgpl -TotalCount 1
            if ($head -notmatch "LESSER GENERAL PUBLIC LICENSE") {
                Add-Fail "licenses\LGPL-3.0.txt does not look like the official GNU LGPL text"
            }
        }
        $gpl = Join-Path $guiRoot "licenses\GPL-3.0.txt"
        if (Test-Path $gpl) {
            $head = Get-Content $gpl -TotalCount 1
            if ($head -notmatch "GNU GENERAL PUBLIC LICENSE") {
                Add-Fail "licenses\GPL-3.0.txt does not look like the official GNU GPL text"
            }
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Error "Package validation failed ($($failures.Count) problem(s))"
}

Write-Host "Package validation PASS"
exit 0
