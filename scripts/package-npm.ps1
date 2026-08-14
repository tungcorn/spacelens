# Stage the npm package from the published unified v0.1.1 zip.
# Never rebuild or substitute a different binary set. Hash mismatch is STOP.

[CmdletBinding()]
param(
    [string]$Version = "",
    [string]$ZipPath = "",
    [string]$OutDir = "",
    [string]$StageDir = "",
    [string]$PinFile = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root "dist" }
if (-not $StageDir) { $StageDir = Join-Path $root "build\npm-package" }
if (-not $PinFile) { $PinFile = Join-Path $root "packaging\npm\release-pin.env" }

function Read-PinFile([string]$Path) {
    if (-not (Test-Path $Path)) {
        Write-Error "release pin not found: $Path"
    }
    $pin = @{}
    foreach ($line in Get-Content $Path) {
        $trim = $line.Trim()
        if (-not $trim -or $trim.StartsWith("#")) { continue }
        $eq = $trim.IndexOf("=")
        if ($eq -lt 1) { continue }
        $pin[$trim.Substring(0, $eq).Trim()] = $trim.Substring($eq + 1).Trim()
    }
    foreach ($key in @(
            "SPACELENS_VERSION", "NPM_PACKAGE_NAME", "RELEASE_TAG",
            "RELEASE_ASSET", "RELEASE_SHA256", "RELEASE_URL", "RELEASE_REPO"
        )) {
        if (-not $pin[$key]) {
            Write-Error "release pin missing $key"
        }
    }
    return $pin
}

function Get-Sha256Lower([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

$pin = Read-PinFile $PinFile
if ($Version -and $Version -ne $pin.SPACELENS_VERSION) {
    Write-Error "requested version '$Version' does not match pin $($pin.SPACELENS_VERSION)"
}
$Version = $pin.SPACELENS_VERSION
$expected = $pin.RELEASE_SHA256.ToLowerInvariant()
if ($expected -ne "b4d4cb993bb53e1414c9fc156d9c29a5dca1b8640ac8d3b1229e5ff5a345793d") {
    Write-Error "release pin SHA-256 is not the immutable published v0.1.1 hash"
}

$template = Join-Path $root "packaging\npm"
$pkgJsonPath = Join-Path $template "package.json"
$pkgJson = Get-Content $pkgJsonPath -Raw | ConvertFrom-Json
if ($pkgJson.name -ne $pin.NPM_PACKAGE_NAME) {
    Write-Error "package.json name '$($pkgJson.name)' does not match pin $($pin.NPM_PACKAGE_NAME)"
}
if ($pkgJson.version -ne $Version) {
    Write-Error "package.json version '$($pkgJson.version)' does not match pin $Version"
}
if ($pkgJson.scripts -and $pkgJson.scripts.postinstall) {
    Write-Error "package.json must not declare postinstall"
}

$cacheDir = Join-Path $root "build\npm-cache"
New-Item -ItemType Directory -Force -Path $cacheDir, $OutDir | Out-Null

if (-not $ZipPath) {
    $ZipPath = Join-Path $cacheDir $pin.RELEASE_ASSET
    $needFetch = $true
    if (Test-Path $ZipPath) {
        $cached = Get-Sha256Lower $ZipPath
        if ($cached -eq $expected) {
            Write-Host "Using cached archive $ZipPath"
            $needFetch = $false
        } else {
            Write-Host "Cached archive hash $cached != $expected; re-downloading"
            Remove-Item -Force $ZipPath
        }
    }
    if ($needFetch) {
        $gh = Get-Command gh -ErrorAction SilentlyContinue
        if ($gh) {
            Write-Host "Downloading $($pin.RELEASE_ASSET) with gh"
            & $gh.Source release download $pin.RELEASE_TAG `
                --repo $pin.RELEASE_REPO `
                --pattern $pin.RELEASE_ASSET `
                --dir $cacheDir `
                --clobber
            if ($LASTEXITCODE -ne 0) {
                Write-Error "gh release download failed"
            }
        } else {
            Write-Host "Downloading $($pin.RELEASE_URL)"
            Invoke-WebRequest -Uri $pin.RELEASE_URL -OutFile $ZipPath -UseBasicParsing
        }
    }
}

if (-not (Test-Path $ZipPath)) {
    Write-Error "release zip not found: $ZipPath"
}

$observed = Get-Sha256Lower $ZipPath
Write-Host "expected SHA-256: $expected"
Write-Host "observed SHA-256: $observed"
if ($observed -ne $expected) {
    Write-Error "STOP: public/local archive hash does not match the immutable v0.1.1 pin"
}

$extract = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-npm-zip-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $extract | Out-Null
try {
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $extract
    $payload = $extract
    $nested = Join-Path $extract ($pin.RELEASE_ASSET -replace '\.zip$', '')
    if (Test-Path (Join-Path $nested "spacelens.exe")) {
        $payload = $nested
    }
    if (-not (Test-Path (Join-Path $payload "spacelens.exe"))) {
        Write-Error "extracted archive missing spacelens.exe"
    }
    if (-not (Test-Path (Join-Path $payload "spacelens-gui.exe"))) {
        Write-Error "extracted archive missing spacelens-gui.exe"
    }

    if (Test-Path $StageDir) {
        Remove-Item -Recurse -Force $StageDir
    }
    $native = Join-Path $StageDir "native"
    $licenses = Join-Path $StageDir "licenses"
    $bin = Join-Path $StageDir "bin"
    New-Item -ItemType Directory -Force -Path $native, $licenses, $bin | Out-Null

    Copy-Item (Join-Path $template "package.json") (Join-Path $StageDir "package.json")
    Copy-Item (Join-Path $template "README.md") (Join-Path $StageDir "README.md")
    Copy-Item (Join-Path $template "bin\*") $bin -Force
    Copy-Item (Join-Path $payload "*") $native -Recurse -Force

    $payloadLicense = Join-Path $payload "LICENSE"
    if (-not (Test-Path $payloadLicense)) {
        Write-Error "published archive missing LICENSE"
    }
    Copy-Item $payloadLicense (Join-Path $StageDir "LICENSE")
    $payloadLicenses = Join-Path $payload "licenses"
    if (Test-Path $payloadLicenses) {
        Copy-Item (Join-Path $payloadLicenses "*") $licenses -Force
    }
    $notices = Join-Path $payload "THIRD_PARTY_NOTICES.txt"
    if (Test-Path $notices) {
        Copy-Item $notices (Join-Path $licenses "THIRD_PARTY_NOTICES.txt")
    }

    & (Join-Path $root "scripts\verify-package.ps1") -MainStage $native
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Push-Location $StageDir
    try {
        Get-ChildItem -Filter "*.tgz" -ErrorAction SilentlyContinue | Remove-Item -Force
        npm pack --ignore-scripts
        if ($LASTEXITCODE -ne 0) {
            Write-Error "npm pack failed"
        }
        $tgz = Get-ChildItem -Filter "*.tgz" | Select-Object -First 1
        if (-not $tgz) {
            Write-Error "npm pack produced no tarball"
        }
        $dest = Join-Path $OutDir $tgz.Name
        if (Test-Path $dest) { Remove-Item -Force $dest }
        Move-Item $tgz.FullName $dest
        Write-Host "npm tarball: $dest"
        Write-Host "staged package: $StageDir"
        Write-Host "native version pin: $Version"
    } finally {
        Pop-Location
    }
} finally {
    Remove-Item -Recurse -Force $extract -ErrorAction SilentlyContinue
}
