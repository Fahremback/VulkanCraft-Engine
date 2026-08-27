@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set ENGINE=C:\Users\fahre\.gemini\antigravity\scratch\vulkan_craft\engine
set FUZZTEST=%ENGINE%\external\solutions\fuzztest
set ABSEIL_SRC=C:\oteltmp\build-gate\_deps\absl-src
set BUILD_DIR=%ENGINE%\out\fuzztest-build

echo [fuzztest] Cleaning...
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"
cd "%BUILD_DIR%"

echo [fuzztest] Writing wrapper CMakeLists.txt...
> "%BUILD_DIR%\CMakeLists.txt" (
    echo cmake_minimum_required^(VERSION 3.20^)
    echo project^(fuzztest_build C CXX^)
    echo set^(CMAKE_CXX_STANDARD 17^)
    echo set^(ABSL_PROPAGATE_CXX_STD ON^)
    echo set^(FUZZTEST_BUILD_TESTING OFF^)
    echo add_subdirectory^("C:/oteltmp/build-gate/_deps/absl-src" abseil_build^)
    echo add_subdirectory^("C:/Users/fahre/.gemini/antigravity/scratch/vulkan_craft/engine/external/solutions/fuzztest" fuzztest_build^)
)

echo [fuzztest] Configuring...
cmake -G "Visual Studio 17 2022" -A x64 -Wno-dev -DCMAKE_SKIP_INSTALL_RULES=ON . 2>&1
if errorlevel 1 (
    echo [fuzztest] CMAKE CONFIG FAILED
    exit /b 1
)

echo [fuzztest] Building fuzztest lib...
cmake --build . --config Release --target fuzztest -- /m /v:minimal 2>&1
echo [fuzztest] Build RC=%ERRORLEVEL%

echo [fuzztest] Finding output...
dir "%BUILD_DIR%\fuzztest_build\Release\*.lib" 2>nul
