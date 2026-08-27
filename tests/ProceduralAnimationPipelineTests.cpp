// ProceduralAnimationPipelineTests — gate do contrato IProceduralAnimationPipeline
// (agente 4 §10 l.169 "pipeline de animação procedural com IK de corpo inteiro,
// foot placement, look-at, aim e constraints"): prova spec all-or-nothing, a
// orquestração das 5 etapas (Legs/Feet/Ik/Aim/Constraints) com diagnóstico por
// etapa, determinismo bit-exact e round-trip JSON.

#include "engine/animation/IProceduralAnimationPipeline.hpp"

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

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

// Terreno plano determinístico em y = 0.
class FlatTerrain : public engine::animation::IFootTerrainSampler {
public:
    engine::animation::SurfaceSample sample(float, float) const override {
        engine::animation::SurfaceSample s;
        s.known = true;
        s.height = 0.0f;
        return s;
    }
};

engine::animation::BonePose bone(const std::string& name, double y = 0.0) {
    engine::animation::BonePose p;
    p.bone = name;
    p.local.position = { 0, y, 0 };
    return p;
}

void test_configure() {
    auto pipeline = engine::animation::create_procedural_animation_pipeline();
    std::string error;

    engine::animation::ProceduralAnimationSpec spec;
    check(pipeline->configure(spec, error), "configure default spec");

    engine::animation::ProceduralAnimationSpec bad;
    bad.footStepLimit = 0.0;
    check(!pipeline->configure(bad, error), "footStepLimit 0 recusa");
    bad = engine::animation::ProceduralAnimationSpec();
    engine::animation::PipelineIkChain c;
    c.rootBone = "hip";
    c.midBone = "hip";  // duplicado
    c.endBone = "foot";
    bad.chains.push_back(c);
    check(!pipeline->configure(bad, error), "chain com ossos duplicados recusa");
    bad = engine::animation::ProceduralAnimationSpec();
    bad.aimBones.push_back("");
    check(!pipeline->configure(bad, error), "aim bone vazio recusa");
    bad = engine::animation::ProceduralAnimationSpec();
    engine::animation::JointLimit l;
    l.bone = "knee";
    l.min_x = 2.0;  // > max_x (default +inf... na verdade min>max)
    l.max_x = 1.0;
    bad.jointLimits.push_back(l);
    check(!pipeline->configure(bad, error), "joint limit min>max recusa");

    // JSON round-trip bit-exact
    engine::animation::ProceduralAnimationSpec a;
    a.footStepLimit = 0.7;
    a.aimBones.push_back("head");
    engine::animation::PipelineIkChain leg;
    leg.kind = engine::animation::IkChainKind::Leg;
    leg.rootBone = "hip";
    leg.midBone = "knee";
    leg.endBone = "foot";
    a.chains.push_back(leg);
    const std::string j1 = a.to_json();
    engine::animation::ProceduralAnimationSpec b;
    check(b.load_from_json(j1, error), "load json");
    check(b.to_json() == j1, "round-trip bit-exact");
    check(b.footStepLimit == 0.7 && b.aimBones.size() == 1 && b.chains.size() == 1,
          "campos restaurados");
    check(!b.load_from_json("{\"footStepLimit\": -1}", error), "load inválido recusa");
}

void test_pipeline_ik() {
    auto pipeline = engine::animation::create_procedural_animation_pipeline();
    std::string error;
    engine::animation::ProceduralAnimationSpec spec;
    engine::animation::PipelineIkChain leg;
    leg.kind = engine::animation::IkChainKind::Leg;
    leg.rootBone = "hip";
    leg.midBone = "knee";
    leg.endBone = "foot";
    leg.jointOffset = { 0, 1, 0 };
    leg.restOffset = { 0, 0, 0.5 };
    spec.chains.push_back(leg);
    spec.footStepLimit = 0.5;
    check(pipeline->configure(spec, error), "configure p/ ik");

    std::vector<engine::animation::BonePose> pose;
    pose.push_back(bone("hip", 1.0));
    pose.push_back(bone("knee"));
    // Pé da pose em y=-0.4 (dentro da janela de passo 0.5 do terreno y=0).
    pose.push_back(bone("foot", -0.4));

    FlatTerrain terrain;
    engine::animation::PipelineBodyState body;
    body.position = { 0, 0, 0 };
    body.yawRadians = 0.0f;

    engine::animation::ProceduralAnimationResult r =
        pipeline->run(body, &terrain, pose, error);
    check(r.ok, "run ok");
    check(r.effectors.size() == 1, "1 efetor");
    check(r.effectors[0].endBone == "foot", "efetor é o foot");
    check(near(r.effectors[0].targetWorld.y, 0.0), "foot ancorado no terreno y=0");
    check(r.diagnostics.size() == 5, "5 diagnósticos (uma por etapa)");
    check(!r.diagnostics[1].stepLimited, "sem step-limited dentro da janela");

    // Pé da pose em y=-1.0 com terreno y=0: salto de 1.0 > janela 0.5 →
    // clamp + stepLimited (o pipeline NUNCA teleporta o pé).
    pose.clear();
    pose.push_back(bone("hip", 1.0));
    pose.push_back(bone("knee"));
    pose.push_back(bone("foot", -1.0));
    r = pipeline->run(body, &terrain, pose, error);
    check(r.ok, "run ok com salto");
    check(r.diagnostics[1].stepLimited, "step-limited com salto 1.0 > janela 0.5");
    check(near(r.effectors[0].targetWorld.y, -0.5), "foot clampado a -0.5");

    // Knee e foot tiveram a rotação tocada pelo shortest-arc (StageIk)
    bool kneeTouched = false, footTouched = false;
    for (std::size_t i = 0; i < r.finalPose.size(); ++i) {
        if (r.finalPose[i].bone == "knee") {
            const auto& q = r.finalPose[i].local.rotation;
            kneeTouched = q.x != 0.0 || q.y != 0.0 || q.z != 0.0 || q.w != 1.0;
        }
        if (r.finalPose[i].bone == "foot") {
            const auto& q = r.finalPose[i].local.rotation;
            footTouched = q.x != 0.0 || q.y != 0.0 || q.z != 0.0 || q.w != 1.0;
        }
    }
    check(kneeTouched, "knee rotacionado pelo IK");
    check(footTouched, "foot rotacionado pelo IK");

    // Pose sem um osso da cadeia → recusa all-or-nothing
    std::vector<engine::animation::BonePose> badPose;
    badPose.push_back(bone("hip", 1.0));
    badPose.push_back(bone("knee"));
    engine::animation::ProceduralAnimationResult br =
        pipeline->run(body, &terrain, badPose, error);
    check(!br.ok, "pose sem foot recusa");
}

void test_aim_and_constraints() {
    auto pipeline = engine::animation::create_procedural_animation_pipeline();
    std::string error;

    // (a) Aim puro — sem constraints: o head rotaciona para o alvo.
    engine::animation::ProceduralAnimationSpec spec;
    spec.aimBones.push_back("head");
    check(pipeline->configure(spec, error), "configure p/ aim");

    std::vector<engine::animation::BonePose> pose;
    pose.push_back(bone("head"));

    engine::animation::PipelineBodyState body;
    body.position = { 0, 0, 0 };
    body.yawRadians = 0.0f;
    body.aimTarget = { 0, 2, 10 };  // alvo à frente e acima
    body.hasAimTarget = true;

    engine::animation::ProceduralAnimationResult r =
        pipeline->run(body, nullptr, pose, error);
    check(r.ok, "run ok com aim");
    const auto& q = r.finalPose[0].local.rotation;
    check(q.x != 0.0 || q.y != 0.0 || q.z != 0.0, "head rotacionado pelo aim");
    check(r.diagnostics[3].bonesTouched == 1, "StageAim tocou 1 osso");

    // (b) Sem aim: head intacto.
    engine::animation::ProceduralAnimationSpec noAim = spec;
    noAim.stageAim = false;
    check(pipeline->configure(noAim, error), "configure sem aim");
    r = pipeline->run(body, nullptr, pose, error);
    const auto& q2 = r.finalPose[0].local.rotation;
    check(q2.x == 0.0 && q2.y == 0.0 && q2.z == 0.0 && q2.w == 1.0,
          "sem aim → head intacto");

    // (c) Constraint pitch 0 DESFAZ a rotação do aim (clamp real).
    engine::animation::ProceduralAnimationSpec con = spec;
    engine::animation::JointLimit l;
    l.bone = "head";
    l.min_x = 0.0;
    l.max_x = 0.0;
    con.jointLimits.push_back(l);
    check(pipeline->configure(con, error), "configure com constraint pitch 0");
    r = pipeline->run(body, nullptr, pose, error);
    const auto& q3 = r.finalPose[0].local.rotation;
    check(q3.x == 0.0 && q3.y == 0.0 && q3.z == 0.0 && q3.w == 1.0,
          "constraint pitch 0 clampa o head de volta");
}

void test_determinism() {
    auto pipeline = engine::animation::create_procedural_animation_pipeline();
    std::string error;
    engine::animation::ProceduralAnimationSpec spec;
    engine::animation::PipelineIkChain leg;
    leg.rootBone = "hip";
    leg.midBone = "knee";
    leg.endBone = "foot";
    leg.jointOffset = { 0, 1, 0 };
    leg.restOffset = { 0, 0, 0.5 };
    spec.chains.push_back(leg);
    spec.aimBones.push_back("head");
    check(pipeline->configure(spec, error), "configure p/ determinismo");

    std::vector<engine::animation::BonePose> pose;
    pose.push_back(bone("hip", 1.0));
    pose.push_back(bone("knee"));
    pose.push_back(bone("foot", -1.0));
    pose.push_back(bone("head"));

    FlatTerrain terrain;
    engine::animation::PipelineBodyState body;
    body.position = { 0, 0, 0 };
    body.aimTarget = { 1, 1, 5 };
    body.hasAimTarget = true;

    const std::string s1 = pipeline->serialize_state();
    engine::animation::ProceduralAnimationResult r1 =
        pipeline->run(body, &terrain, pose, error);
    engine::animation::ProceduralAnimationResult r2 =
        pipeline->run(body, &terrain, pose, error);
    check(r1.finalPose.size() == r2.finalPose.size(), "mesmo tamanho de pose");
    bool bitExact = true;
    for (std::size_t i = 0; i < r1.finalPose.size() && i < r2.finalPose.size(); ++i) {
        const auto& a = r1.finalPose[i].local.rotation;
        const auto& b = r2.finalPose[i].local.rotation;
        if (a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) bitExact = false;
    }
    check(bitExact, "determinismo bit-exact (2 runs)");

    // serialize/deserialize round-trip bit-exact
    auto pipeline2 = engine::animation::create_procedural_animation_pipeline();
    check(pipeline2->deserialize_state(s1, error), "deserialize spec");
    check(pipeline2->serialize_state() == s1, "serialize round-trip bit-exact");
}

}  // namespace

int main() {
    std::printf("ProceduralAnimationPipelineTests\n");
    test_configure();
    test_pipeline_ik();
    test_aim_and_constraints();
    test_determinism();
    if (failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
