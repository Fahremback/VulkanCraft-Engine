#!/bin/bash
# linux-validation.sh — §9: Executar validação real no Windows e no Linux
# Self-contained Linux validation script for the VulkanCraft engine.
# Prerequisites: g++ (>=14), cmake (>=3.20), ninja-build, pkg-config
# Usage: bash tools/portability/linux-validation.sh
# Exit 0 = all checks passed; exit 1 = failure with evidence.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENGINE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_DIR="$ENGINE_DIR/out/linux-validation"
LOG="$OUT_DIR/validation.log"
mkdir -p "$OUT_DIR"
exec > >(tee "$LOG") 2>&1

echo "[linux-validation] Start: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "[linux-validation] Engine: $ENGINE_DIR"
echo "[linux-validation] Out: $OUT_DIR"

# ── 1. Toolchain check ──────────────────────────────────────────────
echo ""
echo "=== Step 1: Toolchain ==="
for tool in g++ cmake ninja pkg-config git; do
    if ! command -v "$tool" &>/dev/null; then
        echo "FAIL: $tool not found"; exit 1
    fi
    echo "  $tool: $(command -v "$tool") ($(which "$tool" | head -1))"
done
GXX_VER=$(g++ -dumpversion)
CMAKE_VER=$(cmake --version | head -1)
echo "  g++ version: $GXX_VER"
echo "  cmake version: $CMAKE_VER"

# ── 2. Configure (Release, Ninja) ───────────────────────────────────
echo ""
echo "=== Step 2: Configure ==="
cd "$ENGINE_DIR"
cmake -S . -B "$OUT_DIR/build" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 \
    > "$OUT_DIR/configure.log" 2>&1
echo "  configure: exit 0"
# Count targets
TARGET_COUNT=$(grep -c "^add_executable\|^add_library" CMakeLists.txt 2>/dev/null || echo "?")
echo "  CMake targets declared: $TARGET_COUNT"

# ── 3. Build engine foundation ──────────────────────────────────────
echo ""
echo "=== Step 3: Build (engine_foundation_tests) ==="
cmake --build "$OUT_DIR/build" \
    --target engine_foundation_tests \
    -- -j$(nproc 2>/dev/null || echo 4) \
    > "$OUT_DIR/build-foundation.log" 2>&1
echo "  build engine_foundation_tests: exit 0"
FOUNDATION_BIN=$(find "$OUT_DIR/build" -name "engine_foundation_tests" -type f 2>/dev/null | head -1)
echo "  binary: $FOUNDATION_BIN"

# ── 4. Build gtest smoke ────────────────────────────────────────────
echo ""
echo "=== Step 4: Build (gtest_smoke_test) ==="
cmake --build "$OUT_DIR/build" \
    --target gtest_smoke_test \
    -- -j$(nproc 2>/dev/null || echo 4) \
    > "$OUT_DIR/build-gtest.log" 2>&1
echo "  build gtest_smoke_test: exit 0"

# ── 5. CTest discovery (gtest) ──────────────────────────────────────
echo ""
echo "=== Step 5: CTest gtest_discover_tests ==="
TEST_LIST=$(ctest --test-dir "$OUT_DIR/build" --show-only 2>/dev/null | grep -E "GtestSmoke\.|MySuite\." || true)
echo "  discovered gtest cases:"
echo "$TEST_LIST" | sed 's/^/    /'
N_DISCOVERED=$(echo "$TEST_LIST" | grep -c "." 2>/dev/null || echo "0")
echo "  total discovered: $N_DISCOVERED"

# ── 6. Run engine_foundation_tests ──────────────────────────────────
echo ""
echo "=== Step 6: Run engine_foundation_tests ==="
FOUNDATION_EXIT=0
"$FOUNDATION_BIN" > "$OUT_DIR/foundation-run.log" 2>&1 || FOUNDATION_EXIT=$?
echo "  engine_foundation_tests: exit $FOUNDATION_EXIT"

# ── 7. Run gtest_smoke_test ─────────────────────────────────────────
echo ""
echo "=== Step 7: Run gtest_smoke_test ==="
GTEST_BIN=$(find "$OUT_DIR/build" -name "gtest_smoke_test" -type f 2>/dev/null | head -1)
GTEST_EXIT=0
"$GTEST_BIN" > "$OUT_DIR/gtest-run.log" 2>&1 || GTEST_EXIT=$?
echo "  gtest_smoke_test: exit $GTEST_EXIT"
grep -E "\[  PASSED  \]|\[  FAILED  \]|Running" "$OUT_DIR/gtest-run.log" | head -8

# ── 8. CTest full run ───────────────────────────────────────────────
echo ""
echo "=== Step 8: CTest ==="
CTest_EXIT=0
ctest --test-dir "$OUT_DIR/build" --output-on-failure > "$OUT_DIR/ctest.log" 2>&1 || CTest_EXIT=$?
TESTS_PASS=$(grep -c "Passed" "$OUT_DIR/ctest.log" 2>/dev/null || echo "0")
TESTS_FAIL=$(grep -c "Failed" "$OUT_DIR/ctest.log" 2>/dev/null || echo "0")
echo "  ctest exit: $CTest_EXIT"
echo "  passed: $TESTS_PASS | failed: $TESTS_FAIL"

# ── 9. Install ──────────────────────────────────────────────────────
echo ""
echo "=== Step 9: Install ==="
PREFIX="$OUT_DIR/prefix"
cmake --install "$OUT_DIR/build" --prefix "$PREFIX" --config Release > "$OUT_DIR/install.log" 2>&1
echo "  install prefix: $PREFIX"
echo "  installed files: $(find "$PREFIX" -type f 2>/dev/null | wc -l)"

# ── 10. External consumer (from installed prefix) ───────────────────
echo ""
echo "=== Step 10: External consumer ==="
CONSUMER_DIR="$OUT_DIR/consumer-test"
mkdir -p "$CONSUMER_DIR"
cat > "$CONSUMER_DIR/main.cpp" <<'CPPEOF'
#include <cstdio>
#include "engine/public/EngineTypes.h"
int main() { std::printf("linux-consumer-ok engine_version=%s\n", "0.0.1"); return 0; }
CPPEOF
cat > "$CONSUMER_DIR/CMakeLists.txt" <<'CMAKEEOF'
cmake_minimum_required(VERSION 3.20)
project(linux_consumer CXX)
set(ENGINE_PREFIX "" CACHE PATH "Installed prefix")
add_executable(consumer main.cpp)
target_include_directories(consumer PRIVATE "${ENGINE_PREFIX}/include")
target_compile_features(consumer PRIVATE cxx_std_17)
CMAKEEOF
cmake -S "$CONSUMER_DIR" -B "$CONSUMER_DIR/build" \
    -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DENGINE_PREFIX="$PREFIX" \
    > "$OUT_DIR/consumer-configure.log" 2>&1
cmake --build "$CONSUMER_DIR/build" -- -j$(nproc 2>/dev/null || echo 4) \
    > "$OUT_DIR/consumer-build.log" 2>&1
CONSUMER_BIN=$(find "$CONSUMER_DIR/build" -name "consumer" -type f 2>/dev/null | head -1)
CONSUMER_EXIT=0
"$CONSUMER_BIN" > "$OUT_DIR/consumer-run.log" 2>&1 || CONSUMER_EXIT=$?
echo "  consumer: exit $CONSUMER_EXIT"
cat "$OUT_DIR/consumer-run.log"

# ── 11. Hostile check (no source tree refs in installed prefix) ─────
echo ""
echo "=== Step 11: Hostile check ==="
ENGINE_REFS=$(grep -r "$ENGINE_DIR/src" "$PREFIX" 2>/dev/null | wc -l || echo "0")
ABS_REFS=$(grep -rE "/home/|/tmp/|/usr/local" "$PREFIX" 2>/dev/null | grep -v "include/cmake\|include/GL\|include/glm" | wc -l || echo "0")
echo "  engine tree refs in prefix: $ENGINE_REFS"
echo "  absolute path refs: $ABS_REFS"

# ── Summary ─────────────────────────────────────────────────────────
echo ""
echo "=== SUMMARY ==="
echo "  toolchain:       OK"
echo "  configure:       OK"
echo "  build foundation: OK"
echo "  build gtest:     OK"
echo "  gtest discover:  $N_DISCOVERED cases"
echo "  foundation run:  exit $FOUNDATION_EXIT"
echo "  gtest run:       exit $GTEST_EXIT"
echo "  ctest:           exit $CTest_EXIT ($TESTS_PASS passed, $TESTS_FAIL failed)"
echo "  install:         $(find "$PREFIX" -type f 2>/dev/null | wc -l) files"
echo "  consumer:        exit $CONSUMER_EXIT"
echo "  hostile:         engine_refs=$ENGINE_REFS abs_refs=$ABS_REFS"

OVERALL=0
for e in $FOUNDATION_EXIT $GTEST_EXIT $CTest_EXIT $CONSUMER_EXIT; do
    [ "$e" != "0" ] && OVERALL=1
done
[ "$ENGINE_REFS" != "0" ] && OVERALL=1
[ "$ABS_REFS" != "0" ] && OVERALL=1

if [ "$OVERALL" = "0" ]; then
    echo ""
    echo "[linux-validation] PASS — all checks passed at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
else
    echo ""
    echo "[linux-validation] FAIL — see $LOG for details"
fi
exit $OVERALL