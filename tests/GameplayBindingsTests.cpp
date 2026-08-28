#include "engine/gameplay/IGameplayBindings.hpp"

#include <cassert>
#include <string>
#include <vector>

int main() {
    using namespace engine::gameplay;
    auto bindings = create_gameplay_bindings();
    std::string error;
    const std::vector<GameplayBinding> domains = {
        {GameplayDomain::Ecs, "ecs", "scene", "save", "replication"},
        {GameplayDomain::Navigation, "nav", "ai", "", ""},
        {GameplayDomain::Ai, "ai", "animation", "", ""},
        {GameplayDomain::Animation, "animation", "renderer", "save", ""},
        {GameplayDomain::Physics, "physics", "gameplay", "", "replication"},
        {GameplayDomain::Voxel, "voxel", "physics", "", "replication"},
        {GameplayDomain::Renderer, "renderer", "editor", "", ""},
        {GameplayDomain::Multiplayer, "network", "gameplay", "save", "replication"},
        {GameplayDomain::Editor, "editor", "scripting", "", ""},
        {GameplayDomain::Scripting, "scripting", "gameplay", "", ""},
        {GameplayDomain::Audio, "audio", "events", "", ""},
        {GameplayDomain::Worlds, "worlds", "portals", "save", "replication"},
        {GameplayDomain::ExternalSolutions, "external", "adapters", "", ""}};
    assert(bindings->configure(domains, error));

    const std::vector<const char*> names = {
        "behavior-tree-cpp", "ceres-solver", "deepmimic", "minecraft-spider",
        "motion-matching", "mujoco", "mujoco-mpc", "ozz-animation", "or-tools",
        "opus", "recast-navigation", "steam-audio", "acl"};
    std::vector<GameplayExternalBinding> external;
    for (const auto* name : names) {
        external.push_back({name, GameplayDomain::ExternalSolutions,
                            std::string("adapter.") + name,
                            std::string("contract.") + name, true});
    }
    assert(bindings->configure_external(external, error));
    assert(bindings->complete(error));
    assert(bindings->bindings().size() == domains.size());
    assert(bindings->external_bindings().size() == 13);
    assert(bindings->to_json().find("behavior-tree-cpp") != std::string::npos);

    auto incomplete = create_gameplay_bindings();
    assert(incomplete->configure(domains, error));
    assert(!incomplete->complete(error));
    assert(error.find("external") != std::string::npos);
    return 0;
}
