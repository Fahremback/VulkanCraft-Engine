@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "C:\Users\fahre\.gemini\antigravity\scratch\vulkan_craft\engine"
cmake --build build --target pbd_solver_tests --config Release -- /t:Rebuild /m:1 2>&1
echo === REBUILD EXIT %ERRORLEVEL% ===
