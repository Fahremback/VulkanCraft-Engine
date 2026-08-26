// PortalSystemTests — gate do contrato IPortalSystem (§6 item 66, portais
// CORE): prova configure/JSON all-or-nothing, find_portal (mais próximo,
// raio, empate por nome), resolve (offset + rotação) e recusas sem mutar.

#include "engine/world/IPortalSystem.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

bool near(float a, float b, float eps = 1.0e-4f) { return (a > b - eps) && (a < b + eps); }

engine::world::PortalLink make_link(const std::string& name,
                                    const std::string& from,
                                    const std::string& to,
                                    float fx, float fz, float tx, float tz) {
    engine::world::PortalLink link;
    link.name = name;
    link.fromWorld = from;
    link.toWorld = to;
    link.fromCenter = { fx, 0.0f, fz };
    link.toCenter = { tx, 0.0f, tz };
    return link;
}

void test_configure_and_find() {
    auto portals = engine::world::create_portal_system();
    std::string error;
    std::vector<engine::world::PortalLink> links;
    links.push_back(make_link("overworld→nether", "overworld", "nether", 10.0f, 20.0f, 100.0f, 200.0f));
    links.push_back(make_link("overworld→end", "overworld", "end", 50.0f, 60.0f, 500.0f, 600.0f));
    check(portals->configure(links, error), "configure 2 links");
    check(portals->link_count() == 2, "2 links");

    engine::world::PortalLink out;
    check(portals->find_portal("overworld", 11.0f, 20.0f, 2.0f, out) &&
              out.name == "overworld→nether",
          "find: mais próximo dentro do raio");
    check(portals->find_portal("overworld", 49.0f, 61.0f, 2.0f, out) &&
              out.name == "overworld→end",
          "find: o outro portal");
    check(!portals->find_portal("overworld", 30.0f, 30.0f, 1.0f, out),
          "find: fora do raio → false");
    check(!portals->find_portal("nether", 11.0f, 20.0f, 2.0f, out),
          "find: mundo sem portal de entrada → false");
}

void test_resolve() {
    auto portals = engine::world::create_portal_system();
    std::string error;
    std::vector<engine::world::PortalLink> links;
    links.push_back(make_link("overworld→nether", "overworld", "nether", 10.0f, 20.0f, 100.0f, 200.0f));
    // Portal com rotação de 90° sobre Y.
    engine::world::PortalLink rotated;
    rotated.name = "a→b";
    rotated.fromWorld = "a";
    rotated.toWorld = "b";
    rotated.fromCenter = { 0.0f, 0.0f, 0.0f };
    rotated.toCenter = { 1000.0f, 0.0f, 1000.0f };
    rotated.rotation = glm::quat(0.7071068f, 0.0f, 0.7071068f, 0.0f);  // 90° Y
    links.push_back(rotated);
    check(portals->configure(links, error), "configure com portal rotacionado");

    glm::vec3 exitPos;
    check(portals->resolve("overworld→nether", { 12.0f, 5.0f, 24.0f }, exitPos),
          "resolve portal simples");
    check(near(exitPos.x, 102.0f) && near(exitPos.y, 5.0f) && near(exitPos.z, 204.0f),
          "offset (2,5,4) → (102,5,204)");

    // Rotação 90° Y: offset (1,0,0) vira (0,0,-1).
    check(portals->resolve("a→b", { 1.0f, 0.0f, 0.0f }, exitPos),
          "resolve portal rotacionado");
    check(near(exitPos.x, 1000.0f) && near(exitPos.z, 999.0f),
          "offset +X rotaciona para -Z (+toCenter)");

    check(!portals->resolve("ghost", { 0.0f, 0.0f, 0.0f }, exitPos),
          "resolve de portal desconhecido → false");
}

void test_all_or_nothing() {
    auto portals = engine::world::create_portal_system();
    std::string error;
    std::vector<engine::world::PortalLink> links;
    links.push_back(make_link("p1", "a", "b", 0.0f, 0.0f, 1.0f, 1.0f));
    check(portals->configure(links, error), "baseline");
    const std::string intact = portals->to_json();

    std::vector<engine::world::PortalLink> bad;
    bad.push_back(make_link("", "a", "b", 0.0f, 0.0f, 1.0f, 1.0f));
    check(!portals->configure(bad, error), "nome vazio recusa");
    bad.clear();
    bad.push_back(make_link("p1", "a", "b", 0.0f, 0.0f, 1.0f, 1.0f));
    bad.push_back(make_link("p1", "b", "c", 0.0f, 0.0f, 1.0f, 1.0f));
    check(!portals->configure(bad, error), "nome duplicado recusa");
    bad.clear();
    bad.push_back(make_link("p2", "a", "a", 0.0f, 0.0f, 1.0f, 1.0f));
    check(!portals->configure(bad, error), "from == to recusa");
    bad.clear();
    bad.push_back(make_link("p2", "", "b", 0.0f, 0.0f, 1.0f, 1.0f));
    check(!portals->configure(bad, error), "mundo vazio recusa");
    bad.clear();
    bad.push_back(make_link("p2", "a", "b", 0.0f, 0.0f, 1.0f, 1.0f));
    bad[0].fromCenter.x = std::nan("");
    check(!portals->configure(bad, error), "centro NaN recusa");
    check(portals->to_json() == intact, "estado intacto após recusas");

    // JSON: malformado / versão errada / sem array.
    check(!portals->load_from_json("{", error), "JSON malformado recusa");
    check(!portals->load_from_json(R"({"version":2,"portals":[]})", error),
          "versão 2 recusa");
    check(!portals->load_from_json(R"({"version":1,"portals":"x"})", error),
          "portals não-array recusa");
    check(portals->to_json() == intact, "estado intacto após recusas JSON");
}

void test_json_roundtrip() {
    auto a = engine::world::create_portal_system();
    auto b = engine::world::create_portal_system();
    std::string error;
    std::vector<engine::world::PortalLink> links;
    links.push_back(make_link("x→y", "x", "y", 5.0f, -3.0f, 9.0f, 2.0f));
    check(a->configure(links, error), "configure A");
    check(b->load_from_json(a->to_json(), error), "load B do JSON de A");
    check(b->to_json() == a->to_json(), "round-trip bit-exact");
    check(b->link_count() == 1 && b->links()[0].name == "x→y",
          "campos preservados");
}

}  // namespace

int main() {
    test_configure_and_find();
    test_resolve();
    test_all_or_nothing();
    test_json_roundtrip();

    if (failures == 0) {
        std::printf("portal_system_tests: all checks passed\n");
        return 0;
    }
    std::printf("portal_system_tests: %d failure(s)\n", failures);
    return 1;
}
