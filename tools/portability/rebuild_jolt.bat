@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\Users\fahre\.gemini\antigravity\scratch\vulkan_craft\engine
cmake -S . -B build
cmake --build build --config Release --target convex_decomposition_tests
