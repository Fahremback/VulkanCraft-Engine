@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "C:\Users\fahre\.gemini\antigravity\scratch\vulkan_craft\engine"
cmake --build build --target vc_pbd --config Release 2>&1
echo === BUILD EXIT %ERRORLEVEL% ===
