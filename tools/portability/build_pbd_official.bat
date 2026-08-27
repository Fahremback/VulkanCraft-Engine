@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "C:\Users\fahre\.gemini\antigravity\scratch\vulkan_craft\engine"
cmake -S . -B build 2>&1 | findstr /v "FetchContent Downloading Updating fatal: No names Progress"
echo === CONFIGURE EXIT %ERRORLEVEL% ===
cmake --build build --target pbd_solver_tests --config Release 2>&1
echo === BUILD EXIT %ERRORLEVEL% ===
