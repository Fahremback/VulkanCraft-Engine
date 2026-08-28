@echo off
setlocal EnableExtensions

set "ENGINE_ROOT=%~dp0..\.."
set "BUILD_SCRIPT=%~dp0build-shared.ps1"
set "TARGET=%~1"
set "JOBS=%~2"
set "MODE=%~3"
set "PAUSE_MODE=%~4"

if not defined TARGET set "TARGET=VulkanEngineGame"
if not defined JOBS set "JOBS=0"
if not exist "%ENGINE_ROOT%\out\artifacts\build" mkdir "%ENGINE_ROOT%\out\artifacts\build"
set "LOG=%ENGINE_ROOT%\out\artifacts\build\build-dev-shared-%TARGET%.log"

if /I "%MODE%"=="--reconfigure" (
    powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%BUILD_SCRIPT%" -Target "%TARGET%" -Jobs "%JOBS%" -Reconfigure -LogPath "%LOG%"
) else (
    powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%BUILD_SCRIPT%" -Target "%TARGET%" -Jobs "%JOBS%" -LogPath "%LOG%"
)
set "EXIT_CODE=%ERRORLEVEL%"

echo.
if "%EXIT_CODE%"=="0" (
    echo BUILD OK: %TARGET%
) else (
    echo BUILD FAILED: %TARGET% ^(exit %EXIT_CODE%^)
)
echo Log: %LOG%
echo.

if /I not "%PAUSE_MODE%"=="--no-pause" pause
exit /b %EXIT_CODE%
