#pragma once

// Allocator.hpp — Optional mimalloc allocator integration.
// mimalloc is 6.7-17.7x faster than the system allocator per benchmark.
// Define VC_USE_MIMALLOC=1 to enable; otherwise uses system allocator.

#ifdef VC_USE_MIMALLOC
#include <mimalloc.h>
#endif

#include <cstddef>
#include <cstdlib>
#include <new>

namespace vc::alloc {

#ifdef VC_USE_MIMALLOC

inline void* allocate(std::size_t bytes) {
    return mi_malloc(bytes);
}

inline void deallocate(void* ptr) {
    mi_free(ptr);
}

inline void* reallocate(void* ptr, std::size_t newBytes) {
    return mi_realloc(ptr, newBytes);
}

inline void install_global() {
    // mimalloc overrides malloc/free via its include header
}

#else

inline void* allocate(std::size_t bytes) {
    return std::malloc(bytes);
}

inline void deallocate(void* ptr) {
    std::free(ptr);
}

inline void* reallocate(void* ptr, std::size_t newBytes) {
    return std::realloc(ptr, newBytes);
}

inline void install_global() {
    // No-op without mimalloc
}

#endif

}  // namespace vc::alloc
