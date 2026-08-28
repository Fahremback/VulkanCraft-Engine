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

    [string]$LogPath
)

$ErrorActionPreference = 'Stop'
$engineRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$resolvedLogPath = $null
if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
    $resolvedLogPath = if ([IO.Path]::IsPathRooted($LogPath)) { $LogPath } else { Join-Path $engineRoot $LogPath }
    $logDirectory = Split-Path -Parent $resolvedLogPath
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
    # Each invocation owns its diagnostic log.  Keeping old compiler output
    # made this file grow indefinitely and slowed every subsequent append.
    Set-Content -LiteralPath $resolvedLogPath -Value $null -Encoding UTF8
}

function Write-BuildMessage {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host $Message
    if ($resolvedLogPath) { Add-Content -LiteralPath $resolvedLogPath -Value $Message }
}

if ($Jobs -eq 0) {
    # Heavy C++ translation units regularly need close to 2 GiB.  CPU-only
    # parallelism overcommits 16 GiB machines and becomes slower from paging.
    $logicalProcessors = [Environment]::ProcessorCount
    $memoryJobs = $logicalProcessors
    try {
        $totalMemory = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
        if ($totalMemory -gt 0) {
            $memoryJobs = [Math]::Max(1, [Math]::Floor($totalMemory / 2GB))
        }
    } catch {
        Write-Verbose "Nao foi possivel consultar RAM; usando apenas a CPU: $($_.Exception.Message)"
    }
    $Jobs = [Math]::Max(1, [Math]::Min(16, [Math]::Min($logicalProcessors, $memoryJobs)))
}
$relativeBuildDir = if ([string]::IsNullOrWhiteSpace($env:VC_BUILD_DIR)) { 'out/dev-shared' } else { $env:VC_BUILD_DIR }
if ([IO.Path]::IsPathRooted($relativeBuildDir) -or $relativeBuildDir -match '(^|[\\/])\.\.([\\/]|$)') {
    throw 'VC_BUILD_DIR must be a repository-relative path.'
}
$buildDir = Join-Path $engineRoot $relativeBuildDir
if ((Split-Path $buildDir -Leaf) -ne 'dev-shared' -and $relativeBuildDir -eq 'out/dev-shared') {
    throw 'The default shared build directory must be out/dev-shared.'
}
# Fase 1 do PLANO_BUILD_RAPIDA.md: cache compartilhado FORA do repositorio.
# Presets só definem variaveis CMake, nao ambiente — o sccache.exe le o
# ambiente do processo, entao o script garante o local para qualquer chamador.
$env:SCCACHE_DIR = Join-Path $env:LOCALAPPDATA 'VulkanCraft/sccache'
$env:SCCACHE_CACHE_TYPE = 'local'
# Ninja recognizes and removes MSVC's English /showIncludes protocol.  A
# localized compiler emits a different prefix, flooding the console/log and
# making dependency scanning substantially slower through sccache.
$env:VSLANG = '1033'
$env:PreferredUILang = 'en-US'
$vcSccacheExe = Join-Path $engineRoot 'tools/portability/sccache.exe'
Remove-Item Env:SCCACHE_NO_DAEMON -ErrorAction SilentlyContinue
$mutex = [System.Threading.Mutex]::new($false, 'Global\VulkanCraft.SharedBuild')
$locked = $false

function Invoke-VcCmake {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Build Tools nao encontrado (vswhere.exe ausente).' }
    $installPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1).Trim()
    if ([string]::IsNullOrWhiteSpace($installPath)) { throw 'Visual Studio Build Tools nao encontrado.' }
    $vcvars = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
    if ([string]::IsNullOrWhiteSpace($installPath) -or -not (Test-Path -LiteralPath $vcvars)) { throw 'MSVC x64 nao encontrado.' }
    $argumentText = ($Arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' '
    Push-Location $engineRoot
    try {
        # vcvars64 establishes PATH, INCLUDE, LIB, LIBPATH, RC and MT together.
        # Do not reconstruct those variables in PowerShell: doing so can select
        # Git's link.exe or hide the Windows SDK resource tools.
        # Start the cache server only after vcvars.  The server snapshots the
        # compiler environment at startup; doing it before vcvars causes C1083
        # on standard headers, while no-daemon mode is unstable under parallel
        # load in sccache 0.10 on Windows.
        $cacheStart = if (Test-Path -LiteralPath $vcSccacheExe) { " && `"$vcSccacheExe`" --start-server >nul 2>&1" } else { '' }
        $command = "call `"$vcvars`" >nul 2>&1 && set `"VSLANG=1033`" && set `"PreferredUILang=en-US`"$cacheStart && cmake.exe $argumentText"
        if ($resolvedLogPath) {
            # Redirect inside cmd.exe.  Windows PowerShell 5 otherwise turns
            # harmless native stderr (for example git/FlatBuffers warnings)
            # into a terminating NativeCommandError before CMake can finish.
            # Ninja already consumed /showIncludes before this pipeline.  Some
            # localized MSVC/sccache combinations still echo every included
            # header (hundreds of thousands of lines), so discard only that
            # presentation noise here to avoid terminal and log I/O overhead.
            & cmd.exe /d /s /c "$command 2>&1" |
                Where-Object { $_ -notmatch '^Observa.*incluindo arquivo:' } |
                Tee-Object -FilePath $resolvedLogPath -Append
        } else {
            & cmd.exe /d /s /c $command
        }
        $cmakeExitCode = $LASTEXITCODE
        if ($cmakeExitCode -ne 0) { throw "CMake falhou ao executar: $argumentText" }
    } finally { Pop-Location }
}

try {
    $locked = $mutex.WaitOne([TimeSpan]::FromSeconds($WaitSeconds))
    if (-not $locked) { throw "A build compartilhada continua ocupada apos $WaitSeconds segundos." }
    if (Test-Path -LiteralPath $vcSccacheExe) {
        # Restart under this build's vcvars environment.  No server is normal.
        & cmd.exe /d /c "`"$vcSccacheExe`" --stop-server >nul 2>&1" | Out-Null
    }
    Write-BuildMessage "VulkanCraft build: target=$Target jobs=$Jobs dir=$relativeBuildDir"
    if ($Reconfigure -or -not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) {
        Invoke-VcCmake @('--preset', 'dev-shared')
    }
    Invoke-VcCmake @('--build', '--preset', 'dev-shared', '--target', $Target, '--parallel', $Jobs)
    # Fase 4 do PLANO_BUILD_RAPIDA.md: estatistica de hit/miss ao final de
    # cada build compartilhada.
    if (Test-Path -LiteralPath $vcSccacheExe) {
        Write-BuildMessage '--- sccache (hit/miss desta maquina) ---'
        & $vcSccacheExe --show-stats | Select-Object -First 8
    }
} finally {
    if ($locked) { $mutex.ReleaseMutex() | Out-Null }
    $mutex.Dispose()
}
