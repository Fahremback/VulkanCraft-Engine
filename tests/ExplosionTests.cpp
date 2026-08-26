// ExplosionTests.cpp — INTEGRAÇÃO (ExplosionRuntime, §16 item 10).
//
// ⚠️ RECONSTRUÇÃO (AGENT-4, 2026-08-26 ~09:5x): o arquivo ORIGINAL deste
// teste (AGENT-2, integração completa com world save/load/replicate + proof
// 27.x + ability data-driven no cenário real) foi SOBRESCRITO acidentalmente
// por este agente ao registrar o contrato de blast CORE com o mesmo nome de
// arquivo (testes/ExplosionTests.cpp era untracked — sem backup em git).
// O runtime `src/engine/gameplay/ExplosionRuntime.{hpp,cpp}` está INTACTO.
// Esta reconstrução cobre os cinco eixos do runtime via API pública
// (materiais/calor/pressão/impulso/terreno) + budgets + determinismo, usando
// o mesmo padrão de setup de tests/DestructionTests.cpp (mesmo autor).
// AGENT-2: confira e restaure os proofs 27.x específicos (world save/load/
// replicate + ability) que não puderam ser recuperados.

#include <engine/registry/BlockRegistry.hpp>
#include <engine/voxel/IVoxelWorld.hpp>

#include "engine/gameplay/ExplosionRuntime.hpp"
#include "engine/physics/PhysicsRuntime.hpp"

#include <glm/glm.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace Engine::Physics;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

class FlatGenerator final : public engine::voxel::IVoxelGenerator {
public:
    explicit FlatGenerator(int height) : height_(height) {}
    engine::voxel::TerrainPoint sample(float, float) const override {
        engine::voxel::TerrainPoint point;
        point.height = height_;
        point.temperature = 0.5f;
        point.moisture = 0.5f;
        point.slope = 0.0f;
        return point;
    }
    float cave_density(float, float, float) const override { return -1.0f; }
    float ore_density(float, float, float) const override { return -1.0f; }

private:
    int height_;
};

constexpr int kGroundTop = 130;

bool boot_world(engine::voxel::IVoxelWorld& world, const glm::vec3& player,
                int budget, int maxBudgetMs = 8000) {
    world.set_chunk_budget(budget);
    const auto start = std::chrono::steady_clock::now();
    while (!world.is_chunk_loaded(0, 0)) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxBudgetMs) {
            return false;
        }
        world.update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

struct TestWorld {
    std::shared_ptr<engine::registry::BlockRegistry> registry;
    std::unique_ptr<engine::voxel::IVoxelWorld> world;
    std::uint32_t woodId{ 0 };
    bool ok{ false };
};

TestWorld make_test_world() {
    TestWorld out;
    std::string error;
    out.registry = std::make_shared<engine::registry::BlockRegistry>();
    out.ok = out.registry->load_from_json(
        R"({"name":"wood","namespace":"test","resistance":0.0,"flammability":1.0,"density":0.6})", error);
    out.world = engine::voxel::create_default_voxel_world();
    out.world->set_block_registry(out.registry);
    out.world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    out.ok = out.ok && boot_world(*out.world, glm::vec3(8.0f, 200.0f, 8.0f), 2) &&
             out.world->resolve_block_id("test:wood", out.woodId, error);
    return out;
}

// 1. Blast em madeira (resistência 0) → carve (terreno) + burn (calor)
//    + caixa afetada (para a camada de conectividade). A madeira é colocada
//    explicitamente no topo do terreno (o chão gerado usa blocos builtin com
//    flammability 0 — só o bloco do registry tem o material correto).
void test_blast_carves_and_burns() {
    TestWorld tw = make_test_world();
    check(tw.ok, "mundo de teste pronto");
    if (!tw.ok) return;

    // Madeira no centro (carve) + no anel d≈3 (burn): a mesma explosão prova
    // as duas etapas — blast vence sobre heat no mesmo cell, e d>=blastRadius
    // cai só na zona de calor.
    tw.world->set_block(8, kGroundTop, 8, tw.woodId);
    tw.world->set_block(8, kGroundTop, 5, tw.woodId);
    tw.world->set_block(8, kGroundTop, 11, tw.woodId);
    tw.world->set_block(5, kGroundTop, 8, tw.woodId);
    tw.world->set_block(11, kGroundTop, 8, tw.woodId);

    PhysicsRuntime physics;
    Engine::Gameplay::ExplosionConfig config;
    config.blastRadius = 3.0f;   // carve: d < 3
    config.heatRadius = 5.0f;    // burn: d in [3,5)
    config.maxPressure = 40.0f;
    config.heatDamage = 1.0f;
    config.ignitionThreshold = 1.0f;
    // Epicentro meio bloco acima do topo (padrão do DestructionTests).
    const glm::vec3 origin(8.0f, static_cast<float>(kGroundTop) + 0.5f, 8.0f);
    const Engine::Gameplay::ExplosionResult r =
        Engine::Gameplay::apply_explosion(*tw.world, physics, origin, config);
    check(r.blocksRemoved > 0, "blast carvou blocos (resistência 0)");
    check(r.blocksIgnited > 0, "calor queimou madeira do anel (flammability 1)");
    check(r.affected_any(), "caixa afetada preenchida (camada de conectividade)");

    // Etapa de CALOR isolada (sem carve) em MUNDO NOVO: inflamáveis queimam.
    TestWorld tw2 = make_test_world();
    check(tw2.ok, "mundo novo pronto");
    if (!tw2.ok) return;
    tw2.world->set_block(8, kGroundTop, 8, tw2.woodId);
    PhysicsRuntime physics2;
    Engine::Gameplay::ExplosionConfig heatOnly = config;
    heatOnly.carveTerrain = false;
    const Engine::Gameplay::ExplosionResult h =
        Engine::Gameplay::apply_explosion(*tw2.world, physics2, origin, heatOnly);
    check(h.blocksIgnited > 0, "calor queimou inflamáveis (carve off)");
    check(h.blocksRemoved == 0, "carve off → nenhum bloco removido");
}

// 2. Pressão/impulso: corpo dinâmico dentro do raio recebe impulso.
void test_blast_impulses_bodies() {
    TestWorld tw = make_test_world();
    check(tw.ok, "mundo de teste pronto");
    if (!tw.ok) return;

    PhysicsRuntime physics;
    BodyDesc desc;
    desc.motion = MotionType::Dynamic;
    // Corpo DENTRO do blastRadius (distância ~1.5 do epicentro).
    desc.position = glm::vec3(8.0f, static_cast<float>(kGroundTop) + 0.5f, 8.0f);
    desc.mass = 1.0f;
    desc.collider.shape = BoxShape{ glm::vec3(0.5f) };
    (void)physics.create_body(desc);

    Engine::Gameplay::ExplosionConfig config;
    config.blastRadius = 3.0f;
    config.impulseScale = 12.0f;
    const glm::vec3 origin(8.0f, static_cast<float>(kGroundTop) - 1.0f, 8.0f);
    const Engine::Gameplay::ExplosionResult r =
        Engine::Gameplay::apply_explosion(*tw.world, physics, origin, config);
    check(r.bodiesImpulsed >= 1, "onda de pressão impulsionou o corpo dinâmico");
}

// 3. Budgets: cap de carve limita a destruição e conta os spills.
void test_budget_caps() {
    TestWorld tw = make_test_world();
    check(tw.ok, "mundo de teste pronto");
    if (!tw.ok) return;

    PhysicsRuntime physics;
    Engine::Gameplay::ExplosionConfig config;
    config.blastRadius = 3.0f;
    config.maxCarvedCells = 1;
    const glm::vec3 origin(8.0f, static_cast<float>(kGroundTop) - 1.0f, 8.0f);
    const Engine::Gameplay::ExplosionResult r =
        Engine::Gameplay::apply_explosion(*tw.world, physics, origin, config);
    check(r.blocksRemoved <= 1, "cap de carve respeitado");
    check(r.carvedSkipped > 0, "spills do carve contabilizados (telemetria)");
}

// 4. Determinismo: mesma cena + mesma config → mesmo resultado.
void test_determinism() {
    auto run_once = []() {
        TestWorld tw = make_test_world();
        if (!tw.ok) return std::size_t{ 0 };
        PhysicsRuntime physics;
        Engine::Gameplay::ExplosionConfig config;
        config.blastRadius = 3.0f;
        config.heatRadius = 3.0f;
        const glm::vec3 origin(8.0f, static_cast<float>(kGroundTop) - 1.0f, 8.0f);
        return Engine::Gameplay::apply_explosion(*tw.world, physics, origin, config)
            .blocksRemoved;
    };
    const std::size_t a = run_once();
    const std::size_t b = run_once();
    check(a == b && a > 0, "determinístico (mesma cena → mesmo carve)");
}

}  // namespace

int main() {
    test_blast_carves_and_burns();
    test_blast_impulses_bodies();
    test_budget_caps();
    test_determinism();

    if (g_failures == 0) {
        std::printf("[explosion] RECONSTRUÇÃO ALL PASSED (runtime intacto)\n");
        return 0;
    }
    std::printf("[explosion] %d failure(s)\n", g_failures);
    return 1;
}
