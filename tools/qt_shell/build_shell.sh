#!/usr/bin/env bash
# build_shell.sh — compila o shell Qt (processo separado, MinGW + Qt 6.6.3).
# Uso:
#   ./build_shell.sh            -> compila QtShell.exe e copia DLLs/plugins
#   ./build_shell.sh smoke      -> compila e roda o smoke (--smoke [porta])
#   ./build_shell.sh smoke 8321 -> smoke contra porta específica
set -euo pipefail

QT="${QT:-/c/Qt/6.6.3/mingw_64}"
MINGW="${MINGW:-/c/msys64/mingw64/bin}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${2:-8321}"

export PATH="$MINGW:$QT/bin:$PATH"

echo "==> compilando QtShell.cpp (MinGW $(g++ --version | head -1))"
g++ -std=c++17 -O2 -fexec-charset=UTF-8 \
    "$HERE/QtShell.cpp" -o "$HERE/QtShell.exe" \
    -I"$QT/include" -I"$QT/include/QtWidgets" -I"$QT/include/QtGui" \
    -I"$QT/include/QtCore" -I"$QT/include/QtNetwork" \
    -L"$QT/lib" -lQt6Widgets -lQt6Gui -lQt6Core -lQt6Network \
    -mwindows

echo "==> copiando runtime Qt (DLLs + platform plugins)"
cp -f "$QT/bin/Qt6Core.dll" "$QT/bin/Qt6Gui.dll" \
      "$QT/bin/Qt6Widgets.dll" "$QT/bin/Qt6Network.dll" "$HERE/"
mkdir -p "$HERE/platforms"
cp -f "$QT/plugins/platforms/qwindows.dll" "$QT/plugins/platforms/qoffscreen.dll" "$HERE/platforms/"

if [[ "${1:-}" == "deploy" ]]; then
    echo "==> empacotando com windeployqt (pasta deploy/)"
    "$QT/bin/windeployqt.exe" --no-translations --no-system-d3d-compiler \
        --no-opengl-sw --dir "$HERE/deploy" "$HERE/QtShell.exe" >/dev/null 2>&1
    cp -f "$HERE/QtShell.exe" "$HERE/deploy/"
    cp -f "$QT/plugins/platforms/qoffscreen.dll" "$HERE/deploy/platforms/"
    echo "==> pacote pronto em deploy/ (inclui qoffscreen p/ smoke headless)"
fi

if [[ "${1:-}" == "smoke" || "${1:-}" == "deploy" ]]; then
    echo "==> smoke headless contra porta $PORT"
    cd "$HERE"
    QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH="$HERE" "$HERE/QtShell.exe" --smoke --port "$PORT"
    echo "exit: $?"
fi
