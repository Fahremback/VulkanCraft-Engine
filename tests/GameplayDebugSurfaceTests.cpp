#include "engine/gameplay/IGameplayDebugSurface.hpp"
#include <cstdio>

int main() {
    auto surface = engine::gameplay::create_gameplay_debug_surface();
    const auto first = surface->to_json();
    const auto second = surface->to_json();
    if (first != second || first.find("\"tick\":0") == std::string::npos) return 1;
    std::printf("gameplay_debug_surface_tests: all checks passed\n");
    return 0;
}
