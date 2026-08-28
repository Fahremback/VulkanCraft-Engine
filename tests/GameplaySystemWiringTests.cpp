#include "engine/gameplay/IGameplaySystemWiring.hpp"

#include <cassert>
#include <string>

int main() {
    auto wiring = engine::gameplay::create_gameplay_system_wiring();
    std::string error;
    engine::gameplay::GameplaySystemWiring empty;
    assert(!wiring->attach(empty, error));
    assert(!error.empty());
    assert(!wiring->complete(error));
    return 0;
}
