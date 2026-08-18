@echo off
setlocal EnableExtensions DisableDelayedExpansion

cd /d "%~dp0"

if not exist ".git" (
    echo [ERRO] Execute este arquivo dentro do repositorio Git.
    pause
    exit /b 1
)

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
    echo [ERRO] Git nao esta disponivel ou este repositorio esta invalido.
    pause
    exit /b 1
)

git add --all
git diff --cached --quiet
if not errorlevel 1 (
    echo Nenhuma alteracao para enviar.
    pause
    exit /b 0
)

set "MESSAGE=%~1"
if "%MESSAGE%"=="" set /p "MESSAGE=Mensagem do commit (Enter para 'chore: update engine'): "
if "%MESSAGE%"=="" set "MESSAGE=chore: update engine"

git commit -m "%MESSAGE%"
if errorlevel 1 (
    echo [ERRO] O commit falhou. Nada foi enviado.
    pause
    exit /b 1
)

git push origin main
if errorlevel 1 (
    echo [ERRO] O envio falhou. O commit ficou salvo localmente.
    pause
    exit /b 1
)

echo.
echo GitHub atualizado com sucesso.
git log -1 --oneline
pause
exit /b 0
