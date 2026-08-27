// mimalloc-probe.cpp — section 7: prove mimalloc as a REAL callable consumer.
#include <mimalloc.h>
#include <cstdio>
#include <cstring>

int main() {
    mi_heap_t* heap = mi_heap_new();
    if (heap == nullptr) { std::fprintf(stderr, "FAIL: mi_heap_new null\n"); return 1; }
    void* p = mi_heap_malloc(heap, 4096);
    if (p == nullptr) { std::fprintf(stderr, "FAIL: mi_heap_malloc null\n"); return 1; }
    std::memset(p, 0xAB, 4096);
    mi_heap_t* owner = mi_heap_of(p);
    if (owner != heap) { std::fprintf(stderr, "FAIL: mi_heap_of not our heap\n"); return 1; }
    if (mi_heap_contains(heap, p) != true) { std::fprintf(stderr, "FAIL: contains rejected\n"); return 1; }
    unsigned char fake = 0;
    if (mi_heap_contains(heap, (void*)&fake) == true) { std::fprintf(stderr, "FAIL: claimed foreign addr\n"); return 1; }
    void* q = mi_heap_realloc(heap, p, 8192);
    if (q == nullptr) { std::fprintf(stderr, "FAIL: realloc null\n"); return 1; }
    if (owner != mi_heap_of(q)) { std::fprintf(stderr, "FAIL: realloc lost owner\n"); return 1; }
    mi_heap_collect(heap, false);
    mi_heap_delete(heap);
    void* g = mi_malloc(128);
    if (g == nullptr) { std::fprintf(stderr, "FAIL: mi_malloc null\n"); return 1; }
    if (mi_heap_of(g) == nullptr) { std::fprintf(stderr, "FAIL: global no heap\n"); return 1; }
    mi_free(g);
    std::printf("mimalloc-consumer-ok heap=1 owner=1 realloc=1 colldel=1 default=1");
    return 0;
}
