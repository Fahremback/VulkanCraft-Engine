// ParticleDrawDataTests — Agente 1 task_plan F.22 (IParticleDrawData): alive
// particle instances must become REAL vertex + indirect draw data consumed by
// the particle pass — not just a count in a UBO. Verifies the quad expansion
// (6 verts per particle), the field-compatible VkDrawIndirectCommand, the
// all-or-nothing validation and the zero-instance no-op.
#include "engine/rendering/IParticleDrawData.hpp"
#include <cassert>
#include <cstdio>

using Engine::Rendering::create_particle_draw_data;
using Engine::Rendering::ParticleDrawVertex;
using Engine::Rendering::ParticleInstance;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::abort();
    }
}

int main() {
    auto batch = create_particle_draw_data();
    check(batch != nullptr, "create_particle_draw_data");

    // Zero instances -> empty draw (legal no-op).
    Engine::Rendering::IndirectDrawCommand cmd;
    std::vector<float> vb;
    std::string err;
    check(batch->build(cmd, vb, err), "build on empty batch succeeds");
    check(cmd.vertexCount == 0, "zero-instance vertexCount==0");
    check(vb.empty(), "zero-instance empty vertex buffer");

    // One particle -> 6 vertices * 8 floats.
    ParticleInstance p;
    p.position = { 1.0f, 2.0f, 3.0f };
    p.size = 0.5f;
    p.color = { 1.0f, 0.5f, 0.25f, 0.75f };
    check(batch->push(p, err), "push valid particle");
    check(batch->count() == 1, "count==1");
    Engine::Rendering::IndirectDrawCommand cmd2;
    std::vector<float> vb2;
    check(batch->build(cmd2, vb2, err), "build one particle");
    check(cmd2.vertexCount == 6, "one particle -> 6 vertices");
    check(cmd2.instanceCount == 1 && cmd2.firstVertex == 0 && cmd2.firstInstance == 0,
          "indirect command fields (VkDrawIndirectCommand layout)");
    check(vb2.size() == 6u * 8u, "vertex buffer = 6 * 8 floats");
    // First vertex carries position, size, color.
    check(vb2[3] == 0.5f, "vertex size attribute");
    check(vb2[5] == 0.5f && vb2[7] == 0.75f, "vertex g/a color");

    // Multiple particles -> vertexCount scales.
    ParticleInstance q;
    q.position = { 0.0f, 0.0f, 0.0f };
    q.size = 1.0f;
    check(batch->push(q, err), "push second particle");
    Engine::Rendering::IndirectDrawCommand cmd3;
    std::vector<float> vb3;
    check(batch->build(cmd3, vb3, err), "build two particles");
    check(cmd3.vertexCount == 12, "two particles -> 12 vertices");
    check(vb3.size() == 12u * 8u, "vertex buffer = 12 * 8 floats");
    check(batch->count() == 2, "count==2");

    // All-or-nothing: invalid size/color refused; nothing partially built.
    ParticleInstance bad;
    bad.size = -1.0f;
    check(!batch->push(bad, err), "negative size refused");
    std::vector<float> vbBefore = vb3;
    check(batch->build(cmd3, vb3, err), "build after intact batch still ok");
    check(vb3 == vbBefore, "intact batch unaffected by refused push");

    // clear resets the batch.
    batch->clear();
    check(batch->count() == 0, "clear resets batch");

    std::printf("ParticleDrawDataTests: all checks passed\n");
    return 0;
}