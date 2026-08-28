#include "engine/capabilities/ICapabilityRegistry.hpp"
#include <cassert>
#include <iostream>
int main() {
    auto r = engine::capabilities::create_capability_registry(); std::string e;
    assert(r->register_capability({"asset.gltf", "glTF", engine::capabilities::CapabilityKind::Asset, "1.0.0", "gltf", false}, e));
    assert(r->register_capability({"service.jobs", "Jobs", engine::capabilities::CapabilityKind::Service}, e));
    assert(r->find("asset.gltf") != nullptr); assert(r->list(engine::capabilities::CapabilityKind::Asset).size() == 1);
    const auto json = r->to_json(); assert(json.find("asset.gltf") != std::string::npos);
    assert(r->load_json(json, e)); assert(!r->register_capability({"asset.gltf", "duplicate"}, e));
    std::cout << "capability-registry-ok\n";
}
