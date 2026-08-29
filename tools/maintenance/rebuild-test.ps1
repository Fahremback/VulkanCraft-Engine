[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9_]+$')]
    [string]$Target,

    [ValidateRange(0, 32)]
    [int]$Jobs = 0,

    [switch]$Reconfigure,

    [ValidateRange(1, 3600)]
    [int]$WaitSeconds = 900,

    [string]$BuildDir = 'out/dev-shared',

    [switch]$NoRun
)

<#
.SYNOPSIS
    rebuild-test.ps1 -Target <tests> : rebuild a test target in the shared
    Ninja build and then run the resulting exe from that build directory
    (or a Release/ subdir if the exe was produced there).

.DESCRIPTION
    Auditor fix for the chronic "stale binary" false failure: a test target
    marked EXCLUDE_FROM_ALL is only relinked when explicitly targeted. When
    another agent rebuilds a vc_sdk_* / vc_* library, the old test exe keeps
    running against the previous ABI and crashes or asserts even though the
    source is correct. Run this helper (rebuild + run) instead of executing
    the stale exe directly or trusting an old ctest log.

    Delegates the build to build-shared.ps1 (vcvars64 + sccache + the shared
    mutex), then locates and executes the produced test binary. Exit code 0
    means build AND run both succeeded (test printed its pass marker); any
    non-zero is reported with the target name so the failure is attributable.
#>

$ErrorActionPreference = 'Stop'
$engineRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path

# 1) Rebuild the target through the shared build (serializes on the global mutex).
$buildScript = Join-Path $PSScriptRoot 'build-shared.ps1'
$buildArgs = @{
    Target       = $Target
    Jobs         = $Jobs
    Reconfigure  = $Reconfigure
    WaitSeconds  = $WaitSeconds
}
& $buildScript @buildArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "rebuild-test: build of target '$Target' failed (exit $LASTEXITCODE)."
    exit 1
}

if ($NoRun) {
    Write-Host "rebuild-test: '$Target' rebuilt OK (skipping run per -NoRun)."
    exit 0
}

# 2) Locate the produced exe. Ninja emits it in the build dir root; a
#    multi-config generator (VS) emits it under a per-config subdir.
$buildDir = Join-Path $engineRoot $BuildDir
$candidates = @(
    (Join-Path $buildDir "$Target.exe"),
    (Join-Path $buildDir "$Target.exe.exe")             # stale-dir habit on Windows
)
# Multi-config generators: check every config subdir that actually exists.
if (Test-Path $buildDir) {
    Get-ChildItem -LiteralPath $buildDir -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName "$Target.exe") } |
        ForEach-Object { $candidates += (Join-Path $_.FullName "$Target.exe") }
}

$exe = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $exe) {
    Write-Error "rebuild-test: '$Target' built but no exe '$Target.exe' found under $buildDir. If the target is a static lib, use -NoRun."
    exit 2
}

Write-Host "rebuild-test: running $exe"
# Run in the exe's directory so relative save/asset paths resolve the same
# way they do under ctest.
$exeDir = Split-Path -Parent $exe
$exeName = Split-Path -Leaf $exe
Push-Location $exeDir
try {
    & (Join-Path $exeDir $exeName)
    $runExit = $LASTEXITCODE
    if ($runExit -ne 0) {
        Write-Host "rebuild-test: '$Target' RUN FAILED with exit $runExit."
    } else {
        Write-Host "rebuild-test: '$Target' rebuild + run OK."
    }
    exit $runExit
} finally {
    Pop-Location
}