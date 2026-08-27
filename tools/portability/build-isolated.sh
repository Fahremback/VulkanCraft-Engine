#!/usr/bin/env bash
# build-isolated.sh — AGENT-6: build/test numa árvore ISOLADA (out/agent-6-iso),
# fora do build/ disputado pelos agentes. Automaticamente:
#   1. configura offline (reusa deps FetchContent de build/_deps)  [se falta cache]
#   2. builda o alvo
#   3. roda o exe (se --run)
# Uso: build-isolated.sh <target> [--run]   ex.: build-isolated.sh destruction_tests --run
set -u
ENGINE="$(cd "$(dirname "$0")/../.." && pwd)"
VCVARS="/c/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat"
ISO="$ENGINE/out/agent-6-iso"
TARGET="${1:?target required}"
RUN="${2:-}"
export MSYS_NO_PATHCONV=1   # don't mangle Windows args under cmd
# 1. Configure if missing
if [ ! -f "$ISO/CMakeCache.txt" ]; then
  echo "[build-isolated] configuring offline build in $ISO ..."
  cmd //c "$(cygpath -w "$VCVARS") && cmake --preset release -B out/agent-6-iso -DFETCHCONTENT_SOURCE_DIR_GLFW=$ENGINE/build/_deps/glfw-src -DFETCHCONTENT_SOURCE_DIR_GLM=$ENGINE/build/_deps/glm-src -DFETCHCONTENT_SOURCE_DIR_IMGUI=$ENGINE/build/_deps/imgui-src -DFETCHCONTENT_SOURCE_DIR_MINIAUDIO=$ENGINE/build/_deps/miniaudio-src -DFETCHCONTENT_SOURCE_DIR_VK-BOOTSTRAP=$ENGINE/build/_deps/vk-bootstrap-src -DFETCHCONTENT_SOURCE_DIR_VMA=$ENGINE/build/_deps/vma-src" .. > /dev/null 2>&1
  # fallback: run the bat
fi
# 2. Build
echo "[build-isolated] building $TARGET in $ISO ..."
cmd //c "$(cygpath -w "$VCVARS") && cmake --build $(cygpath -w "$ISO") --target $TARGET"
rc=$?
# 3. Run
if [ "$RUN" = "--run" ]; then
  for exe in "$ISO/$TARGET.exe" "$ISO/$TARGET"; do
    if [ -x "$exe" ]; then echo "[build-isolated] running $exe"; "$exe"; echo "[build-isolated] run exit: $?"; break; fi
  done
fi
exit $rc
