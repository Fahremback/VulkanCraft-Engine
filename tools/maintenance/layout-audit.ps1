param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
$rootPath = (Resolve-Path -LiteralPath $Root).Path

$generatedFiles = @(
    'CMakeCache.txt', 'CTestTestfile.cmake', 'cmake_install.cmake',
    'build_version.cc', 'imgui.ini'
)
$generatedDirectories = @(
    '_deps', 'CMakeFiles', 'Debug', 'Release', 'RelWithDebInfo', 'Testing',
    'flatbuffers_build', 'rocksdb_build', 'x64', 'ALL_BUILD.dir',
    'INSTALL.dir', 'RUN_TESTS.dir', 'ZERO_CHECK.dir'
)
$allowedRootFiles = @(
    '.gitignore', 'atualizar_github.bat', 'CMakeLists.txt', 'CMakePresets.json',
    'FALTANTES.md', 'findings.md', 'progress.md', 'README.md', 'task_plan.md'
)
$allowedRootDirectories = @(
    'agentes', 'assets', 'cmake', 'docs', 'external', 'generated', 'out',
    'Projects', 'schema', 'shaders', 'src', 'tests', 'third_party', 'tools'
)

$issues = [System.Collections.Generic.List[object]]::new()
foreach ($entry in Get-ChildItem -LiteralPath $rootPath -Force) {
    $name = $entry.Name
    if ($entry.PSIsContainer) {
        if ($generatedDirectories -contains $name -or $name -like '*.dir') {
            $issues.Add([pscustomobject]@{ Kind='generated-directory'; Path=$entry.FullName })
        } elseif ($name -like 'build*') {
            $issues.Add([pscustomobject]@{ Kind='legacy-build-directory'; Path=$entry.FullName })
        } elseif ($allowedRootDirectories -notcontains $name) {
            $issues.Add([pscustomobject]@{ Kind='unexpected-directory'; Path=$entry.FullName })
        }
        continue
    }

    if ($generatedFiles -contains $name -or
        $name -like '*.vcxproj' -or $name -like '*.vcxproj.filters' -or
        $name -like '*.obj' -or $name -like '*.sln' -or $name -like '*.slnx') {
        $issues.Add([pscustomobject]@{ Kind='generated-file'; Path=$entry.FullName })
    } elseif ($entry.Extension -in @('.png', '.log')) {
        $issues.Add([pscustomobject]@{ Kind='loose-artifact'; Path=$entry.FullName })
    } elseif ($allowedRootFiles -notcontains $name) {
        $issues.Add([pscustomobject]@{ Kind='unexpected-file'; Path=$entry.FullName })
    }
}

$report = [pscustomobject]@{
    Root = $rootPath
    Clean = ($issues.Count -eq 0)
    IssueCount = $issues.Count
    Issues = $issues
}

if ($Json) {
    $report | ConvertTo-Json -Depth 4
} else {
    Write-Output "LAYOUT_ROOT=$rootPath"
    Write-Output "LAYOUT_CLEAN=$($report.Clean)"
    Write-Output "LAYOUT_ISSUES=$($issues.Count)"
    $issues | Sort-Object Kind,Path | Format-Table -AutoSize
}

if ($issues.Count -ne 0) { exit 1 }
