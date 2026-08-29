// FuzzTestSmokeTests.cpp — gate headless do runtime de fuzzing vendido
// (Google FuzzTest, external/solutions/fuzztest, Apache-2.0).
//
// Consumidor REAL do target `vc_fuzztest`: define propriedades FUZZ_TEST do
// domínio da engine (coordenadas de chunk voxel, parser de manifestos
// "key=value", checksum FNV-ish e conversão global->local de blocos) e roda
// em unit-test mode (determinístico, sem libFuzzer). Cada propriedade é
// executada com milhares de entradas geradas pelo motor de domínios do
// FuzzTest; a propriedade só passa se valer para TODAS elas.
//
// Nota de portabilidade (2026-08-29, Agente 4 ajudando Agente 6 / BUG-045):
// InRegexp/InGrammar requerem RE2 (não vendido) e ficam desligados via
// FUZZTEST_HAVE_RE2 (indefinido); subprocess.cc (POSIX-only) e
// flatbuffers_domain são excluídos do build. O main replica o padrão de
// fuzztest_gtest_main.cc (o gate fornece main() próprio).

#include "fuzztest/fuzztest.h"
#include "fuzztest/init_fuzztest.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Propriedade 1 — coordenada de chunk nunca sai dos limites após clamp
// (espelha a matemática de world->chunk da engine: alcance ±range).
// ---------------------------------------------------------------------------
int32_t ClampChunkCoord(int32_t coord, int32_t range) {
  const int32_t r = (range < 1) ? 1 : range;
  return std::max<int32_t>(-r, std::min<int32_t>(r, coord));
}

void ChunkCoordStaysInBounds(int32_t coord, int32_t range) {
  const int32_t r = (range < 1) ? 1 : range;
  const int32_t out = ClampChunkCoord(coord, range);
  ASSERT_GE(out, -r);
  ASSERT_LE(out, r);
  // Clamp é idempotente — segunda aplicação não muda nada.
  ASSERT_EQ(out, ClampChunkCoord(out, range));
}
FUZZ_TEST(FuzzSmoke, ChunkCoordStaysInBounds);

// ---------------------------------------------------------------------------
// Propriedade 2 — parser de manifesto "key=value" nunca crasha e nunca
// produz entrada para linha malformada (all-or-nothing, estilo engine).
// ---------------------------------------------------------------------------
bool ParseManifestLine(const std::string& line, std::string& key, int& value) {
  key.clear();
  value = 0;
  const auto eq = line.find('=');
  if (eq == std::string::npos || eq == 0) return false;
  const std::string k = line.substr(0, eq);
  const std::string v = line.substr(eq + 1);
  if (k.empty() || v.empty()) return false;
  if (k.size() > 64 || v.size() > 16) return false;
  for (char c : k) {
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
        !(c >= '0' && c <= '9') && c != '_') {
      return false;
    }
  }
  bool neg = false;
  std::size_t i = 0;
  if (v[0] == '-') {
    neg = true;
    i = 1;
    if (v.size() == 1) return false;
  }
  int64_t acc = 0;
  for (; i < v.size(); ++i) {
    if (v[i] < '0' || v[i] > '9') return false;
    acc = acc * 10 + (v[i] - '0');
    if (acc > 1000000000ll) return false;  // estouro de int32 evitado
  }
  value = static_cast<int>(neg ? -acc : acc);
  key = k;
  return true;
}

void ManifestParserNeverCrashes(const std::string& line) {
  std::string key;
  int value = 0;
  // Deve retornar false ou produzir uma entrada consistente — nunca crashar,
  // nunca estourar int, nunca aceitar linha vazia/chave vazia.
  const bool ok = ParseManifestLine(line, key, value);
  if (ok) {
    ASSERT_FALSE(key.empty());
    ASSERT_FALSE(line.substr(0, line.find('=')).empty());
  } else {
    ASSERT_TRUE(key.empty());
    ASSERT_EQ(value, 0);
  }
}
FUZZ_TEST(FuzzSmoke, ManifestParserNeverCrashes);

// ---------------------------------------------------------------------------
// Propriedade 3 — checksum FNV-ish é estável e livre de UB
// (mesma entrada => mesmo hash, em qualquer ordem de execução).
// ---------------------------------------------------------------------------
uint32_t FoldChecksum(const std::vector<int32_t>& data) {
  uint32_t h = 2166136261u;
  for (int32_t v : data) {
    h ^= static_cast<uint32_t>(v);
    h *= 16777619u;
  }
  return h;
}

void ChecksumFoldIsStable(const std::vector<int32_t>& data) {
  const uint32_t h = FoldChecksum(data);
  ASSERT_EQ(h, FoldChecksum(data));
}
FUZZ_TEST(FuzzSmoke, ChecksumFoldIsStable);

// ---------------------------------------------------------------------------
// Propriedade 4 — conversão global->local de bloco dentro de um chunk 32^3
// nunca estoura e sempre reconstrói a coordenada global (round-trip).
// ---------------------------------------------------------------------------
constexpr int32_t kChunkSize = 32;

void BlockCoordRoundTrip(int32_t world_x, int32_t world_y, int32_t world_z) {
  // chunk index e local offset (aritmética usada pela engine, floor-div).
  auto floor_div = [](int32_t v, int32_t d) -> int32_t {
    int32_t q = v / d;
    if ((v % d) < 0) --q;
    return q;
  };
  const int32_t cx = floor_div(world_x, kChunkSize);
  const int32_t cy = floor_div(world_y, kChunkSize);
  const int32_t cz = floor_div(world_z, kChunkSize);
  const int32_t lx = world_x - cx * kChunkSize;
  const int32_t ly = world_y - cy * kChunkSize;
  const int32_t lz = world_z - cz * kChunkSize;
  ASSERT_GE(lx, 0);
  ASSERT_LT(lx, kChunkSize);
  ASSERT_GE(ly, 0);
  ASSERT_LT(ly, kChunkSize);
  ASSERT_GE(lz, 0);
  ASSERT_LT(lz, kChunkSize);
  ASSERT_EQ(world_x, cx * kChunkSize + lx);
  ASSERT_EQ(world_y, cy * kChunkSize + ly);
  ASSERT_EQ(world_z, cz * kChunkSize + lz);
}
FUZZ_TEST(FuzzSmoke, BlockCoordRoundTrip);

}  // namespace

// main() próprio (o target vc_fuzztest NÃO embute fuzztest_gtest_main.cc).
int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  fuzztest::InitFuzzTest(&argc, &argv);
  return RUN_ALL_TESTS();
}
