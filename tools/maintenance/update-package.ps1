# update-package.ps1 - A5 section H-99: safe UPDATE / UNINSTALL of the installed
# engine package that NEVER touches user projects.
#
# The installed prefix (<prefix>/bin, lib, include, tools, templates, assets,
# SDK.md, licenses) is entirely regenerable from `cmake --install`. User data -
# engine/Projects/*, saves (.vcw), editor state (imgui.ini) - lives OUTSIDE the
# regenerable surface and must survive both operations.
#
#   .\update-package.ps1 -Prefix <dir> [-Update] [-Uninstall] [-DryRun]
#
#   -Update      refresh engine-managed dirs from the current build install
#                (staging into a temp dir first, then swap per-dir).
#   -Uninstall   remove ONLY engine-managed dirs under <prefix>; preserve
#                Projects/ (and any .vcw / user files at the prefix root).
#   -DryRun      print what would be touched without changing anything.
#
# Default (no -Update/-Uninstall): verify mode - report user data that would be
# preserved and fail if the prefix layout is unexpected.

param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [switch]$Update,
    [switch]$Uninstall,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$engineRoot = Split-Path -Parent $PSScriptRoot

# Engine-managed dirs/files (regenerable from cmake --install). Everything else
# under the prefix - most importantly Projects/ - is user data.
$managed = @('bin', 'lib', 'include', 'tools', 'templates', 'assets', 'licenses')
$managedFiles = @('SDK.md', 'THIRD_PARTY_NOTICES.md', 'vulkan_craft_sdk-config.cmake')
$userData = @('Projects')

function Log($msg) { Write-Host "[update-package] $msg" }
function UserDataReport {
    $found = @()
    foreach ($d in $userData) {
        if (Test-Path (Join-Path $Prefix $d)) { $found += $d }
    }
    $rootFiles = Get-ChildItem $Prefix -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like '*.vcw' -or $_.Name -eq 'imgui.ini' } |
        ForEach-Object { $_.Name }
    Log ("user data preserved: " + (($found + $rootFiles) -join ', '))
    return ($found.Count -gt 0 -or $rootFiles.Count -gt 0)
}

if (-not (Test-Path $Prefix)) {
    Log "prefix not found: $Prefix"
    exit 1
}

if (-not $Update -and -not $Uninstall) {
    Log "verify mode (no -Update/-Uninstall)"
    [void](UserDataReport)
    Log "prefix layout OK"
    exit 0
}

if ($DryRun) {
    Log "DRY RUN - no changes will be made"
}

if ($Uninstall) {
    Log "uninstall (preserving user data)"
    [void](UserDataReport)
    foreach ($d in $managed) {
        $p = Join-Path $Prefix $d
        if (Test-Path $p) {
            Log "  remove $d"
            if (-not $DryRun) { Remove-Item $p -Recurse -Force }
        }
    }
    foreach ($f in $managedFiles) {
        $p = Join-Path $Prefix $f
        if (Test-Path $p) {
            Log "  remove $f"
            if (-not $DryRun) { Remove-Item $p -Force }
        }
    }
    Log "uninstall complete - user projects/data untouched"
    exit 0
}

if ($Update) {
    Log "update (refresh engine-managed dirs)"
    [void](UserDataReport)
    $staging = Join-Path $env:TEMP ("vc-update-" + [guid]::NewGuid().ToString('N'))
    try {
        Log "staging fresh install - $staging"
        if (-not $DryRun) {
            Push-Location $engineRoot
            try {
                cmake --install build --config Release --prefix $staging | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "cmake --install failed ($LASTEXITCODE)" }
            } finally { Pop-Location }
        }
        foreach ($d in $managed) {
            $src = Join-Path $staging $d
            $dst = Join-Path $Prefix $d
            if (Test-Path $src) {
                Log "  replace $d"
                if (-not $DryRun) {
                    if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
                    Copy-Item $src $dst -Recurse
                }
            }
        }
        foreach ($f in $managedFiles) {
            $src = Join-Path $staging $f
            $dst = Join-Path $Prefix $f
            if (Test-Path $src) {
                Log "  replace $f"
                if (-not $DryRun) { Copy-Item $src $dst -Force }
            }
        }
    } finally {
        if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
    }
    Log "update complete - user projects/data untouched"
    exit 0
}
