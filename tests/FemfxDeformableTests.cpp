// FemfxDeformableTests — gate do provedor FEM truss atrás de
// IDeformableProvider (DeformableProviderKind::Femfx). Fecha o item
// G.femfx: a contraparte headless/determinística do FEMFX (AMD) — FEM
// linear de treliça implementada do zero no SDK, sem o SDK proprietário.
//
// Prova: a fábrica cria o Femfx (em vez de recusar), cadeia pendurada
// estica sob gravidade até equilíbrio (deformação pequena e positiva),
// nó ancorado nunca move, per-edge stiffness desativa o elemento, piso
// com restituição, determinismo bit-exact e validação all-or-nothing.

#include "engine/deformable/IDeformableProvider.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

using Engine::Deformable::DeformableConfig;
using Engine::Deformable::DeformableMeshDesc;
using Engine::Deformable::DeformableProviderKind;
using Engine::Deformable::IDeformableProvider;
using Engine::Deformable::InvalidDeformableBody;
using Engine::Deformable::create_deformable_provider;

// Cadeia vertical suspensa (fora do piso): nó 0 ancorado, 3 nós livres,
// 3 arestas (treliça). Com o piso em y=0, o nó livre assenta ao ser
// desacoplado; suspenso, estica sob gravidade.
DeformableMeshDesc make_chain() {
    DeformableMeshDesc desc;
    desc.nodes = { glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, 4.0f, 0.0f),
                   glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f, 2.0f, 0.0f) };
    desc.edges = { { 0, 1 }, { 1, 2 }, { 2, 3 } };
    desc.fixed = { true, false, false, false };
    return desc;
}

void test_factory_and_validation() {
    DeformableConfig config;
    std::string error;
    std::unique_ptr<IDeformableProvider> femfx =
        create_deformable_provider(DeformableProviderKind::Femfx, config,
                                   error);
    check(femfx != nullptr, "factory creates the Femfx provider");
    check(femfx->kind() == DeformableProviderKind::Femfx, "kind is Femfx");

    // Config inválida recusada (all-or-nothing).
    DeformableConfig bad = config;
    bad.stiffness = 0.0f;
    check(create_deformable_provider(DeformableProviderKind::Femfx, bad,
                                     error) == nullptr &&
              !error.empty(),
          "stiffness 0 refused");

    // Malha sem arestas recusada (elemento de treliça ausente).
    DeformableMeshDesc noEdges;
    noEdges.nodes = { glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f) };
    check(femfx->create_body(noEdges, error) == InvalidDeformableBody &&
              !error.empty(),
          "mesh without edges refused");

    // Aresta com índice inválido recusada.
    DeformableMeshDesc badEdge = make_chain();
    badEdge.edges = { { 0, 5 } };
    check(femfx->create_body(badEdge, error) == InvalidDeformableBody &&
              !error.empty(),
          "invalid edge index refused");
}

void test_hanging_chain_equilibrium() {
    DeformableConfig config;
    config.stiffness = 0.8f;
    config.substeps = 4;
    config.solverIterations = 4;
    config.damping = 0.05f;
    std::string error;
    auto femfx = create_deformable_provider(DeformableProviderKind::Femfx,
                                            config, error);
    check(femfx != nullptr, "provider created");
    const auto body = femfx->create_body(make_chain(), error);
    check(body != InvalidDeformableBody, "chain body created");
    check(femfx->node_count(body) == 4, "four nodes");

    // Âncora nunca move.
    check(femfx->node_position(body, 0) == glm::vec3(0.0f, 5.0f, 0.0f),
          "anchored node never moves");
    check(femfx->node_velocity(body, 0) == glm::vec3(0.0f),
          "anchored node velocity is zero");

    // Cadeia suspensa estica (deformação pequena e positiva) sob gravidade:
    // o nó inferior desce um pouco abaixo do repouso (y = 2).
    for (int i = 0; i < 600; ++i) femfx->step(0.004f);
    const glm::vec3 bottom = femfx->node_position(body, 3);
    check(bottom.y < 2.0f, "truss stretches downward under gravity");
    check(bottom.y > 1.5f, "chain does not collapse (stiffness holds)");
    const float err = femfx->constraint_error(body);
    check(err > 0.0f && err < 0.05f,
          "truss strain is small and positive at equilibrium");
    // Em equilíbrio as velocidades são ~0 (damping).
    check(glm::length(femfx->node_velocity(body, 3)) < 1e-3f,
          "equilibrium reached (bottom node nearly at rest)");
}

void test_edge_stiffness_deactivation() {
    DeformableConfig config;
    config.stiffness = 0.8f;
    config.damping = 0.001f;  // queda quase livre p/ o nó desacoplado
    std::string error;
    auto femfx = create_deformable_provider(DeformableProviderKind::Femfx,
                                            config, error);
    const auto body = femfx->create_body(make_chain(), error);
    check(body != InvalidDeformableBody, "body created");

    // Desativa a aresta inferior (separação): o nó 3 desce livre.
    femfx->set_edge_stiffness(body, 2, 0.0f);
    for (int i = 0; i < 300; ++i) femfx->step(0.004f);
    const glm::vec3 bottom = femfx->node_position(body, 3);
    // Nó 3 só sofre gravidade (piso em y = 0, restituição 0 -> assenta).
    check(std::fabs(bottom.y - config.groundY) < 1e-4f,
          "deactivated element: free node settles on the ground");
}

void test_determinism() {
    DeformableConfig config;
    config.stiffness = 0.6f;
    std::string error;
    auto a = create_deformable_provider(DeformableProviderKind::Femfx,
                                        config, error);
    auto b = create_deformable_provider(DeformableProviderKind::Femfx,
                                        config, error);
    const auto ha = a->create_body(make_chain(), error);
    const auto hb = b->create_body(make_chain(), error);
    check(ha != InvalidDeformableBody && hb != InvalidDeformableBody,
          "bodies created");
    for (int i = 0; i < 100; ++i) {
        a->step(0.004f);
        b->step(0.004f);
    }
    bool identical = true;
    for (std::uint32_t n = 0; n < 4; ++n) {
        if (a->node_position(ha, n) != b->node_position(hb, n) ||
            a->node_velocity(ha, n) != b->node_velocity(hb, n))
            identical = false;
    }
    check(identical, "two identical FEM bodies step bit-identically");
}

}  // namespace

int main() {
    test_factory_and_validation();
    test_hanging_chain_equilibrium();
    test_edge_stiffness_deactivation();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "femfx_deformable_tests: all checks passed\n";
        return 0;
    }
    std::cout << "femfx_deformable_tests: " << g_failures << " failure(s)\n";
    return 1;
}
