[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9_]+$')]
    [string]$Target,

    [ValidateRange(1, 32)]
    [int]$Jobs = 2,

    [switch]$Reconfigure,

    [ValidateRange(1, 3600)]
    [int]$WaitSeconds = 900
)

$ErrorActionPreference = 'Stop'
$engineRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$relativeBuildDir = if ([string]::IsNullOrWhiteSpace($env:VC_BUILD_DIR)) { 'out/dev-shared' } else { $env:VC_BUILD_DIR }
if ([IO.Path]::IsPathRooted($relativeBuildDir) -or $relativeBuildDir -match '(^|[\\/])\.\.([\\/]|$)') {
    throw 'VC_BUILD_DIR must be a repository-relative path.'
}
$buildDir = Join-Path $engineRoot $relativeBuildDir
if ((Split-Path $buildDir -Leaf) -ne 'dev-shared' -and $relativeBuildDir -eq 'out/dev-shared') {
    throw 'The default shared build directory must be out/dev-shared.'
}
$mutex = [System.Threading.Mutex]::new($false, 'Global\VulkanCraft.SharedBuild')
$locked = $false

function Invoke-VcCmake {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Build Tools nao encontrado (vswhere.exe ausente).' }
    $installPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1).Trim()
    $vcvars = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
    if ([string]::IsNullOrWhiteSpace($installPath) -or -not (Test-Path -LiteralPath $vcvars)) { throw 'MSVC x64 nao encontrado.' }
    $argumentText = ($Arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' '
    Push-Location $engineRoot
    try {
        & cmd.exe /d /s /c "call `"$vcvars`" >nul && cmake.exe $argumentText"
        if ($LASTEXITCODE -ne 0) { throw "CMake falhou ao executar: $argumentText" }
    } finally { Pop-Location }
}

try {
    $locked = $mutex.WaitOne([TimeSpan]::FromSeconds($WaitSeconds))
    if (-not $locked) { throw "A build compartilhada continua ocupada apos $WaitSeconds segundos." }
    if ($Reconfigure -or -not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) {
        Invoke-VcCmake @('--preset', 'dev-shared')
    }
    Invoke-VcCmake @('--build', '--preset', 'dev-shared', '--target', $Target, '--parallel', $Jobs)
} finally {
    if ($locked) { $mutex.ReleaseMutex() | Out-Null }
    $mutex.Dispose()
}
