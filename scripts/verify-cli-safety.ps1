# Agent-boundary gate for a built SpaceLens CLI.
# Fails if capabilities JSON is invalid, filesystem_mutation is not false,
# or a destructive / safety-write verb is accepted.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CliPath
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $CliPath)) {
    Write-Error "CLI not found: $CliPath"
}

$exe = (Resolve-Path $CliPath).Path
Write-Host "Verifying CLI safety: $exe"

$raw = & $exe capabilities --json
if ($LASTEXITCODE -ne 0) {
    Write-Error "capabilities --json exited $LASTEXITCODE"
}
if ([string]::IsNullOrWhiteSpace($raw)) {
    Write-Error "capabilities --json produced no output"
}

try {
    $caps = $raw | ConvertFrom-Json
} catch {
    Write-Error "capabilities --json is not valid JSON: $_`n$raw"
}

if ($null -eq $caps.filesystem_mutation) {
    Write-Error "capabilities JSON missing filesystem_mutation"
}
if ($caps.filesystem_mutation -ne $false) {
    Write-Error "filesystem_mutation is '$($caps.filesystem_mutation)', expected false"
}
if ($caps.features -and $caps.features.filesystem_mutation -ne $false) {
    Write-Error "features.filesystem_mutation is not false"
}
if ($caps.read_only -ne $true) {
    Write-Error "read_only is '$($caps.read_only)', expected true"
}

$versionLine = & $exe version
if ($LASTEXITCODE -ne 0) {
    Write-Error "version exited $LASTEXITCODE"
}
Write-Host "version: $versionLine"

$destructive = @(
    @("delete"),
    @("remove"),
    @("rm"),
    @("move"),
    @("purge"),
    @("wipe"),
    @("recycle"),
    @("restore"),
    @("maintenance"),
    @("keep-one"),
    @("cleanup", "--execute"),
    @("duplicates", "--delete"),
    @("trust"),
    @("allow-root"),
    @("ordinary-root"),
    @("safety", "--write")
)

$probe = Join-Path ([System.IO.Path]::GetTempPath()) ("spacelens-safety-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $probe | Out-Null
try {
    foreach ($verb in $destructive) {
        $output = & $exe @verb $probe 2>&1 | Out-String
        $code = $LASTEXITCODE
        if ($code -eq 0) {
            Write-Error "Destructive verb accepted (exit 0): $($verb -join ' ')"
        }
        if ($output -match '(?i)recycled|deleted|moved|restored|purged') {
            Write-Error "Destructive verb produced a mutation-like message: $($verb -join ' ')`n$output"
        }
        Write-Host "rejected: $($verb -join ' ') (exit $code)"
    }
} finally {
    Remove-Item -Recurse -Force $probe -ErrorAction SilentlyContinue
}

Write-Host "CLI safety gate PASS (filesystem_mutation=false)"
exit 0
