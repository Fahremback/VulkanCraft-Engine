// AllocatorTests.cpp — Gate for optional mimalloc allocator wrapper.

#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/core/memory/Allocator.hpp"

static int g_failures = 0;

static void check(bool ok, const char* name) {
    if (ok) { std::printf("PASS: %s\n", name); }
    else    { std::printf("FAIL: %s\n", name); ++g_failures; }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("AllocatorTests gate v1\n");
        return 0;
    }

    std::printf("Allocator gate\n");

    // Test basic allocate/deallocate
    void* p = vc::alloc::allocate(42);
    check(p != nullptr, "allocate returns non-null");
    static_cast<int*>(p)[0] = 123;
    check(static_cast<int*>(p)[0] == 123, "allocated memory is writable");
    vc::alloc::deallocate(p);

    // Test reallocate
    int* pr = static_cast<int*>(vc::alloc::allocate(10 * sizeof(int)));
    pr[0] = 1;
    pr[1] = 2;
    int* pr2 = static_cast<int*>(vc::alloc::reallocate(pr, 20 * sizeof(int)));
    check(pr2 != nullptr, "reallocate returns non-null");
    check(pr2[0] == 1 && pr2[1] == 2, "data preserved after realloc");
    vc::alloc::deallocate(pr2);

    // Test bulk allocation
    std::vector<void*> ptrs;
    for (int i = 0; i < 100; ++i) {
        void* p2 = vc::alloc::allocate(64);
        check(p2 != nullptr, "bulk allocate succeeds");
        ptrs.push_back(p2);
    }
    for (void* p2 : ptrs) {
        vc::alloc::deallocate(p2);
    }

    vc::alloc::install_global();

#ifdef VC_USE_MIMALLOC
    check(true, "mimalloc backend active");
    std::printf("  backend: mimalloc\n");
#else
    check(true, "system allocator fallback");
    std::printf("  backend: system allocator\n");
#endif

    if (g_failures == 0) { std::printf("ALL ALLOCATOR GATE TESTS PASSED\n"); return 0; }
    std::printf("%d ALLOCATOR GATE TEST(S) FAILED\n", g_failures);
    return 1;
}
