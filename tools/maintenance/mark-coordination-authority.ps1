param(
    [string]$EngineRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'
$marker = '<!-- VULKANCRAFT-EXECUTION-AUTHORITY -->'
$referenceNotice = @'
$marker
> **CONTEXTO TÉCNICO:** a execução é regida por `task_plan.md`, `agentes/FONTE_UNICA.md`, `agentes/EXECUCAO_FINAL_5_AGENTES.md` e pelo plano atual do agente.
'@
$referenceNotice = $referenceNotice.Replace('$marker', $marker)
$legacyNotice = @'
$marker
> **ARQUIVO LEGADO — NÃO EXECUTAR:** use `task_plan.md`, `agentes/FONTE_UNICA.md`, `agentes/EXECUCAO_FINAL_5_AGENTES.md` e o plano atual do agente.
'@
$legacyNotice = $legacyNotice.Replace('$marker', $marker)
$planNotice = @'
$marker
> **PLANO ATUAL AUTORITATIVO:** execute este checklist sob as regras de `task_plan.md`, `agentes/FONTE_UNICA.md` e `agentes/EXECUCAO_FINAL_5_AGENTES.md`.
'@
$planNotice = $planNotice.Replace('$marker', $marker)
$bugsNotice = @'
$marker
> **REGISTRO DE BUGS APENAS:** a autoridade está em `task_plan.md`, `agentes/FONTE_UNICA.md`, `agentes/EXECUCAO_FINAL_5_AGENTES.md` e no plano deste agente.
'@
$bugsNotice = $bugsNotice.Replace('$marker', $marker)

$referenceFiles = @(
    'task_plan.md',
    'FALTANTES.md',
    'findings.md',
    'progress.md',
    'agentes/PENDENCIAS.md',
    'docs/AI_SWARM_STATUS.md',
    'docs/MIGRATION_STATUS.md',
    'docs/REVIEW_2026_08.md',
    'docs/README_AUDIT.md',
    'docs/INDEX.md',
    'docs/SOLUCOES_E_DEPENDENCIAS.md',
    'docs/META_ENGINE_SANDBOX_UNIVERSAL.md',
    'docs/ARCHITECTURE.md',
    'docs/EDITOR_FRONTEND_WICKED_EZENGINE.md',
    'docs/ACELERADORES_ANIMACAO_FISICA_PROCGEN.md',
    'docs/TEXT_ONLY_VALIDATION.md',
    'docs/PROJECT_LAYOUT.md',
    'src/editor/frontend/PORTS.md',
    'agentes/EXECUCAO_FINAL_5_AGENTES.md'
)

$legacyFiles = Get-ChildItem -LiteralPath (Join-Path $EngineRoot 'agentes\legacy') -Filter '*.md' -File -Recurse |
    ForEach-Object { [IO.Path]::GetRelativePath($EngineRoot, $_.FullName).Replace('\', '/') }
$currentPlanFiles = Get-ChildItem -LiteralPath (Join-Path $EngineRoot 'agentes') -Filter 'task_plan.md' -File -Recurse |
    Where-Object { $_.FullName -notmatch '[\\/]legacy[\\/]' } |
    ForEach-Object { [IO.Path]::GetRelativePath($EngineRoot, $_.FullName).Replace('\', '/') }
$bugFiles = Get-ChildItem -LiteralPath (Join-Path $EngineRoot 'agentes') -Filter 'bugs.md' -File -Recurse |
    Where-Object { $_.FullName -notmatch '[\\/]legacy[\\/]' } |
    ForEach-Object { [IO.Path]::GetRelativePath($EngineRoot, $_.FullName).Replace('\', '/') }

function Add-AuthorityNotice([string]$relativePath, [string]$notice) {
    $path = Join-Path $EngineRoot $relativePath
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Arquivo de coordenação esperado não existe: $relativePath"
    }

    $content = [IO.File]::ReadAllText($path)
    $newline = if ($content.Contains("`r`n")) { "`r`n" } else { "`n" }
    $normalizedNotice = $notice.Replace("`r`n", "`n").Replace("`n", $newline)
    if ($content.Contains($marker)) {
        $pattern = [regex]::Escape($marker) + '[^\r\n]*(?:\r?\n)>[^\r\n]*'
        $updated = [regex]::Replace($content, $pattern, [Text.RegularExpressions.MatchEvaluator]{ param($match) $normalizedNotice }, 1)
        if ($updated -eq $content) { return $false }
        [IO.File]::WriteAllText($path, $updated, [Text.UTF8Encoding]::new($false))
        return $true
    }

    $firstBreak = $content.IndexOf($newline)
    if ($firstBreak -ge 0 -and $content.StartsWith('#')) {
        $updated = $content.Insert($firstBreak + $newline.Length, $newline + $normalizedNotice + $newline)
    } else {
        $updated = $normalizedNotice + $newline + $newline + $content
    }

    [IO.File]::WriteAllText($path, $updated, [Text.UTF8Encoding]::new($false))
    return $true
}

$changed = 0
foreach ($file in $referenceFiles) {
    if (Add-AuthorityNotice $file $referenceNotice) { $changed++ }
}
foreach ($file in $legacyFiles) {
    if (Add-AuthorityNotice $file $legacyNotice) { $changed++ }
}
foreach ($file in $currentPlanFiles) {
    if (Add-AuthorityNotice $file $planNotice) { $changed++ }
}
foreach ($file in $bugFiles) {
    if (Add-AuthorityNotice $file $bugsNotice) { $changed++ }
}

Write-Output "authority-files=$($referenceFiles.Count + $legacyFiles.Count + $currentPlanFiles.Count + $bugFiles.Count) changed=$changed"
