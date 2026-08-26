param(
    [string]$EngineRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'
$marker = '<!-- VULKANCRAFT-EXECUTION-AUTHORITY -->'
$referenceNotice = @'
$marker
> **AUTORIDADE DE EXECUÇÃO:** este arquivo é requisito, evidência ou histórico. Antes de agir, leia `agentes/FONTE_UNICA.md`, `agentes/EXECUCAO_TOTAL_6_AGENTES.md` e o `task_plan.md` atual do seu agente. Em conflito, eles vencem. Este arquivo não transfere tarefas nem autoriza `DONE`.
'@
$referenceNotice = $referenceNotice.Replace('$marker', $marker)
$legacyNotice = @'
$marker
> **ARQUIVO LEGADO — NÃO EXECUTAR:** use `agentes/FONTE_UNICA.md`, `agentes/EXECUCAO_TOTAL_6_AGENTES.md` e o `task_plan.md` atual do seu agente. Nomes, donos, estados e tarefas deste arquivo não possuem autoridade atual.
'@
$legacyNotice = $legacyNotice.Replace('$marker', $marker)
$planNotice = @'
$marker
> **PLANO ATUAL AUTORITATIVO:** execute este checklist sob as regras de `agentes/FONTE_UNICA.md` e `agentes/EXECUCAO_TOTAL_6_AGENTES.md`. Nenhum outro arquivo ou mensagem pode remover, transferir ou concluir itens daqui.
'@
$planNotice = $planNotice.Replace('$marker', $marker)
$bugsNotice = @'
$marker
> **REGISTRO DE BUGS APENAS:** a autoridade está em `agentes/FONTE_UNICA.md`, `agentes/EXECUCAO_TOTAL_6_AGENTES.md` e no `task_plan.md` deste agente. Um bug não transfere nem encerra a tarefa correspondente.
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
    'agentes/agente6_integracao/successor_gameplay_ai.md',
    'agentes/agente6_integracao/handoff_agente5.md'
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
