@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set ENGINE=C:\Users\fahre\.gemini\antigravity\scratch\vulkan_craft\engine
set GNS_DIR=%ENGINE%\external\solutions\game-networking-sockets
set BUILD_DIR=%GNS_DIR%\build-gns
set PROBE_DIR=%ENGINE%\tools\portability
set PB_DIR=C:\oteltmp\build-gate\_deps\protobuf-build
set ABSEIL_DIR=%ENGINE%\external\solutions\abseil

echo [gns] Cleaning...
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"

echo [gns] Configuring with BCrypt + protobuf + abseil...
cmake -G "Visual Studio 17 2022" -A x64 -Wno-dev ^
  -DCMAKE_SKIP_INSTALL_RULES=ON ^
  -DUSE_CRYPTO=BCrypt ^
  -DBUILD_TESTING=OFF ^
  -DProtobuf_DIR="%PB_DIR%\lib\cmake\protobuf" ^
  -DProtobuf_INCLUDE_DIR="%PB_DIR%\..\protobuf-src\src" ^
  -DProtobuf_LIBRARY="%PB_DIR%\Release\libprotobuf.lib" ^
  -Dabsl_DIR="%ABSEIL_DIR%\lib\cmake\absl" ^
  -DABSL_PROPAGATE_CXX_STD=ON ^
  -S "%GNS_DIR%" -B "%BUILD_DIR%" 2>&1

if errorlevel 1 (
    echo [gns] Trying without absl_DIR hint...
    cmake -G "Visual Studio 17 2022" -A x64 -Wno-dev ^
      -DCMAKE_SKIP_INSTALL_RULES=ON ^
      -DUSE_CRYPTO=BCrypt ^
      -DBUILD_TESTING=OFF ^
      -DProtobuf_DIR="%PB_DIR%\lib\cmake\protobuf" ^
      -DProtobuf_INCLUDE_DIR="%PB_DIR%\..\protobuf-src\src" ^
      -DProtobuf_LIBRARY="%PB_DIR%\Release\libprotobuf.lib" ^
      -DCMAKE_PREFIX_PATH="%ABSEIL_DIR%;%PB_DIR%" ^
      -S "%GNS_DIR%" -B "%BUILD_DIR%" 2>&1
    if errorlevel 1 (
        echo [gns] CMAKE CONFIG FAILED
        exit /b 1
    )
)

echo [gns] Configured! Building Release ALL...
cmake --build "%BUILD_DIR%" --config Release -- /m /v:minimal 2>&1
set BUILD_RC=%ERRORLEVEL%
echo [gns] Build RC=%BUILD_RC%

echo.
echo [gns] Finding output...
for /r "%BUILD_DIR%" %%f in (GameNetworkingSockets*.dll) do echo DLL: %%f
for /r "%BUILD_DIR%" %%f in (GameNetworkingSockets*.lib) do echo LIB: %%f

echo.
echo [gns-probe] Compiling...
cl /EHsc /nologo /std:c++17 /DSTEAMNETWORKINGSOCKETS_STATIC_LINK /DWIN32 /D_WINDOWS ^
   /I"%GNS_DIR%\include" /I"%GNS_DIR%\include\steam" /I"%BUILD_DIR%" ^
   "%PROBE_DIR%\gns-probe.cpp" /Fe:"%PROBE_DIR%\gns-probe.exe" 2>&1

if errorlevel 1 (
    echo [gns-probe] COMPILE FAILED
    exit /b 1
)

echo [gns-probe] Linking...

REM Try DLL import lib (preferred — full API)
for %%f in ("%BUILD_DIR%\src\Release\GameNetworkingSockets.lib") do (
    echo [gns-probe] Import lib: %%f
    link /nologo /out:"%PROBE_DIR%\gns-probe.exe" "%PROBE_DIR%\gns-probe.obj" "%%f" ws2_32.lib advapi32.lib 2>&1
    if not errorlevel 1 goto :run_probe
)

REM Try static lib
for %%f in ("%BUILD_DIR%\src\Release\GameNetworkingSockets_s.lib") do (
    echo [gns-probe] Static lib: %%f
    link /nologo /out:"%PROBE_DIR%\gns-probe.exe" "%PROBE_DIR%\gns-probe.obj" "%%f" ws2_32.lib advapi32.lib 2>&1
    if not errorlevel 1 goto :run_probe
)

echo [gns-probe] ALL LINK ATTEMPTS FAILED
exit /b 1

:run_probe
echo [gns-probe] Running...
"%PROBE_DIR%\gns-probe.exe"
echo RUN_RC=%ERRORLEVEL%
