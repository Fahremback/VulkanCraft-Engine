// immer-probe.cpp — §7 (immer): the vendored immer library is header-only and
// needs no CMake wiring to be USED — this probe compiles against the vendored
// headers directly (external/solutions/immer/immer/) and exercises the
// persistent data structures the editor wants for undo/snapshots/timeline.
// Build (C++17):
//   g++ -std=c++17 -I external/solutions/immer tools/portability/immer-probe.cpp -o /tmp/immer-probe
// Exit 0 = the vendored library is usable; non-zero = a probe failure.
//
// Exercises: vector push_back/update (persistent), flex_vector (segmented),
// box (atomic value semantics), atom (thread-safe). All deterministic, no IO.
#include <cstdio>
#include <cstdint>

#include "immer/vector.hpp"
#include "immer/flex_vector.hpp"
#include "immer/box.hpp"
#include "immer/atom.hpp"

int main() {
  // immer::vector — persistent vector, structural sharing on update.
  auto v = immer::vector<int>{};
  for (int i = 0; i < 1000; ++i) v = v.push_back(i);
  if (v.size() != 1000) return 1;
  if (v[0] != 0 || v[999] != 999) return 2;
  // update() returns a NEW vector; the old one is unchanged (persistence).
  auto w = v.update(0, [](int x) { return x + 1; });
  if (v[0] != 0 || w[0] != 1) return 3;
  if (w.size() != 1000) return 4;

  // immer::flex_vector — segmented vector, supports push_front too.
  auto f = immer::flex_vector<std::int64_t>{};
  for (int i = 0; i < 256; ++i) f = f.push_back(i);
  f = f.push_front(-1);
  if (f.size() != 257) return 5;
  if (f[0] != -1 || f[256] != 255) return 6;

  // immer::box — immutable value semantics with reference counting.
  auto b = immer::box<std::int64_t>{42};
  auto c = b;
  if (b.get() != 42 || c.get() != 42) return 7;
  c = c.update([](auto) { return 43; });
  if (b.get() != 42 || c.get() != 43) return 8;

  // immer::atom — thread-safe atomic box (single-threaded use here).
  auto a = immer::atom<std::int64_t>{1};
  a.update([](auto x) { return x + 1; });
  if (a.load() != 2) return 9;

  std::printf("immer-probe OK (vector 1000 persistent, flex_vector 257, box/atom value semantics)\n");
  return 0;
}
