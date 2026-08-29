// MotionMatchVendorTests — gate do contrato IMotionMatchVendor (§8
// motion-matching, DEPENDENCY_POLICY): prova o core REAL do clone vendido
// (external/solutions/motion-matching) atrás da superfície pública — banco de
// poses com features normalizadas (pés/quadril/trajetória via FK do doador),
// busca por vizinho mais próximo com custo de transição, determinismo e
// recusas all-or-nothing. O teste usa a FK vendida (forward_kinematics) para
// montar o estado mundial das consultas, garantindo consistência com o banco.

#include "engine/animation/IMotionMatchVendor.hpp"

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

constexpr int kNBones = 9;  // 0 root, 1 hips, 2-5 perna E, 6-8 perna D
constexpr int kParents[kNBones] = { -1, 0, 1, 2, 3, 4, 1, 6, 7 };

// Gera o frame i de uma caminhada sintética: root avança em +X com yaw
// senoidal; pés alternam balanço em Z. Features de todos os grupos variam
// (o doador exige std > 0 por grupo).
engine::animation::VendorPose make_pose(int i) {
    engine::animation::VendorPose pose;
    const float x = static_cast<float>(i) * 0.1f;
    const float yaw = 0.1f * std::sin(i * 0.03);
    const float swingL = 0.15f * std::sin(i * 0.1f);
    const float swingR = -0.15f * std::sin(i * 0.1f);

    const float posData[kNBones][3] = {
        { x, 1.0f, 0.0f },        // root
        { 0.0f, 0.0f, 0.0f },     // hips
        { 0.0f, -0.05f, 0.05f },  // left up leg
        { 0.0f, -0.5f, 0.0f },    // left leg
        { 0.0f, -0.45f, swingL },  // left foot
        { 0.0f, 0.0f, 0.05f },    // left toe
        { 0.0f, -0.05f, -0.05f }, // right up leg
        { 0.0f, -0.5f, 0.0f },    // right leg
        { 0.0f, -0.45f, swingR },  // right foot
    };
    const float velData[kNBones][3] = {
        { 0.1f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.15f * 0.1f * std::cos(i * 0.1f) },
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, -0.15f * 0.1f * std::cos(i * 0.1f) },
    };
    for (int b = 0; b < kNBones; ++b) {
        pose.bonePositions.insert(pose.bonePositions.end(),
                                  { posData[b][0], posData[b][1], posData[b][2] });
        // Rotação: root com yaw em Y; demais identidade. Guardado xyzw.
        const float half = yaw * (b == 0 ? 1.0f : 0.0f) * 0.5f;
        const float c = std::cos(half), s = std::sin(half);
        if (b == 0) {
            pose.boneRotations.insert(pose.boneRotations.end(), { 0.0f, s, 0.0f, c });
        } else {
            pose.boneRotations.insert(pose.boneRotations.end(), { 0.0f, 0.0f, 0.0f, 1.0f });
        }
        pose.boneVelocities.insert(pose.boneVelocities.end(),
                                   { velData[b][0], velData[b][1], velData[b][2] });
        pose.boneParents.push_back(kParents[b]);
    }
    return pose;
}

// quat_mul_vec3: rota o vetor v pela quat q (xyzw), reproduzindo a convenção
// da matemática vendida (quat.h) usada na FK do doador.
void rotate_vec3(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    // t = 2 * cross(xyz, v); out = v + w*t + cross(xyz, t)
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// Estado MUNDIAL do frame replicando a FK recursiva do doador (database.h):
//   pos(b) = quat(parent_rot) * local_pos(b) + parent_pos;  vel(b) idem.
// As rotações não-root da caminhada sintética são identidade, então a única
// rotação que atua é a do root (yaw). As rotações mundiais não-root, com todos
// os pais identidade, são também identidade — exatamente o que a FK do doador
// computa, tornando as features da consulta idênticas às do banco.
void world_state(const std::vector<engine::animation::VendorPose>& poses,
                 int frame, std::vector<float>& positions,
                 std::vector<float>& rotations, std::vector<float>& velocities) {
    const engine::animation::VendorPose& pose = poses[static_cast<std::size_t>(frame)];
    positions.assign(kNBones * 3, 0.0f);
    rotations.assign(kNBones * 4, 0.0f);
    velocities.assign(kNBones * 3, 0.0f);
    auto local3 = [&pose](int b) {
        return pose.bonePositions.data() + b * 3;
    };
    auto localV3 = [&pose](int b) {
        return pose.boneVelocities.data() + b * 3;
    };
    // Na FK do doador, como TODAS as rotações locais não-root são identidade,
    // a rotação mundial de QUALQUER osso é o produto das rotações da cadeia = a
    // rotação do root (yaw), aplicada recursivamente a cada offset local.
    // Então: mundo(b) = rootPos + R0 * Σ offsets locais da cadeia, e o mesmo
    // para velocidade (angular invariante → sem termo de cross).
    const float* rootRot = pose.boneRotations.data();
    for (int b = 0; b < kNBones; ++b) {
        if (pose.boneParents[static_cast<std::size_t>(b)] < 0) {
            // root: pos/vel = local direto; rotação = local (yaw).
            positions[static_cast<std::size_t>(b * 3 + 0)] = local3(b)[0];
            positions[static_cast<std::size_t>(b * 3 + 1)] = local3(b)[1];
            positions[static_cast<std::size_t>(b * 3 + 2)] = local3(b)[2];
            velocities[static_cast<std::size_t>(b * 3 + 0)] = localV3(b)[0];
            velocities[static_cast<std::size_t>(b * 3 + 1)] = localV3(b)[1];
            velocities[static_cast<std::size_t>(b * 3 + 2)] = localV3(b)[2];
        } else {
            // Soma os offsets locais da cadeia (b → filho do root).
            float off[3] = { 0.0f, 0.0f, 0.0f };
            float offVel[3] = { 0.0f, 0.0f, 0.0f };
            int node = b;
            while (node > 0) {
                off[0] += local3(node)[0]; off[1] += local3(node)[1]; off[2] += local3(node)[2];
                offVel[0] += localV3(node)[0]; offVel[1] += localV3(node)[1]; offVel[2] += localV3(node)[2];
                node = pose.boneParents[static_cast<std::size_t>(node)];
            }
            float roff[3], roffVel[3];
            rotate_vec3(rootRot, off, roff);
            rotate_vec3(rootRot, offVel, roffVel);
            const float* rp = local3(0);
            const float* rv = localV3(0);
            positions[static_cast<std::size_t>(b * 3 + 0)] = rp[0] + roff[0];
            positions[static_cast<std::size_t>(b * 3 + 1)] = rp[1] + roff[1];
            positions[static_cast<std::size_t>(b * 3 + 2)] = rp[2] + roff[2];
            velocities[static_cast<std::size_t>(b * 3 + 0)] = rv[0] + roffVel[0];
            velocities[static_cast<std::size_t>(b * 3 + 1)] = rv[1] + roffVel[1];
            velocities[static_cast<std::size_t>(b * 3 + 2)] = rv[2] + roffVel[2];
        }
        // Rotação mundial: root com yaw; demais identidade (FK com pais identidade).
        if (b == 0) {
            const float* r = pose.boneRotations.data();
            rotations[static_cast<std::size_t>(b * 4 + 0)] = r[0];
            rotations[static_cast<std::size_t>(b * 4 + 1)] = r[1];
            rotations[static_cast<std::size_t>(b * 4 + 2)] = r[2];
            rotations[static_cast<std::size_t>(b * 4 + 3)] = r[3];
        } else {
            rotations[static_cast<std::size_t>(b * 4 + 3)] = 1.0f;
        }
    }
}

engine::animation::VendorQuery make_query(
    const std::vector<engine::animation::VendorPose>& poses, int frame) {
    engine::animation::VendorQuery query;
    std::vector<float> pos, rot, vel;
    world_state(poses, frame, pos, rot, vel);
    query.worldPositions = pos;
    query.worldRotations = rot;
    query.worldVelocities = vel;
    // Trajetória futura: root (osso 0) mundo em +20/+40/+60 frames (clampado
    // ao fim do banco, como o database_trajectory_index_clamp do doador).
    const int last = static_cast<int>(poses.size()) - 1;
    for (int t : { 20, 40, 60 }) {
        const int idx = frame + t <= last ? frame + t : last;
        const engine::animation::VendorPose& f = poses[static_cast<std::size_t>(idx)];
        const float* p = f.bonePositions.data();
        const float* r = f.boneRotations.data();
        // FK do root com yaw: posição = local, rotação = local (root sem pai).
        query.trajectoryPositions.insert(query.trajectoryPositions.end(),
                                         { p[0], p[1], p[2] });
        query.trajectoryRotations.insert(query.trajectoryRotations.end(),
                                         { r[0], r[1], r[2], r[3] });
    }
    return query;
}

void test_build_and_sizes() {
    std::vector<engine::animation::VendorPose> poses;
    for (int i = 0; i < 120; ++i) poses.push_back(make_pose(i));

    std::string error;
    auto vendor = engine::animation::create_motion_match_vendor();
    check(vendor->build_database(poses, error), "build 120 poses");
    check(vendor->frame_count() == 120, "frame_count 120");
    check(vendor->feature_count() == 27, "feature_count 27");
}

void test_exact_match() {
    std::vector<engine::animation::VendorPose> poses;
    for (int i = 0; i < 120; ++i) poses.push_back(make_pose(i));

    std::string error;
    auto vendor = engine::animation::create_motion_match_vendor();
    check(vendor->build_database(poses, error), "build para busca");

    for (int frame : { 20, 60, 100 }) {
        engine::animation::VendorQuery query = make_query(poses, frame);
        std::int32_t frameIndex = -1;
        float cost = -1.0f;
        check(vendor->query(query, -1, 0.0f, frameIndex, cost, error),
              "query frame exato");
        std::printf("    [diag] frame %d → best %d, cost %.6f\n", frame,
                    frameIndex, cost);
        check(frameIndex == frame, "busca retorna o frame exato");
        check(cost >= 0.0f && cost < 0.01f, "custo ~0 para match exato");
    }
}

void test_determinism() {
    std::vector<engine::animation::VendorPose> poses;
    for (int i = 0; i < 120; ++i) poses.push_back(make_pose(i));

    std::string error;
    auto vendor = engine::animation::create_motion_match_vendor();
    check(vendor->build_database(poses, error), "build p/ determinismo");

    engine::animation::VendorQuery query = make_query(poses, 40);
    std::int32_t a = -1, b = -1;
    float ca = -1.0f, cb = -1.0f;
    check(vendor->query(query, -1, 0.0f, a, ca, error), "query 1");
    check(vendor->query(query, -1, 0.0f, b, cb, error), "query 2");
    check(a == b && ca == cb, "busca determinística (mesmo frame e custo)");
}

void test_transition_cost() {
    std::vector<engine::animation::VendorPose> poses;
    for (int i = 0; i < 120; ++i) poses.push_back(make_pose(i));

    std::string error;
    auto vendor = engine::animation::create_motion_match_vendor();
    check(vendor->build_database(poses, error), "build p/ transição");

    engine::animation::VendorQuery query = make_query(poses, 80);
    std::int32_t frameIndex = -1;
    float cost = -1.0f;
    // currentFrame válido + custo de transição: o doador adiciona a penalidade
    // de continuidade; o resultado continua sendo um frame válido e >= 0.
    check(vendor->query(query, 79, 0.5f, frameIndex, cost, error),
          "query com custo de transição");
    check(frameIndex >= 0 && frameIndex < 120 && cost >= 0.0f,
          "frame e custo válidos com transição");
}

void test_rejections() {
    std::string error;
    auto vendor = engine::animation::create_motion_match_vendor();

    std::int32_t frameIndex = -1;
    float cost = -1.0f;
    check(!vendor->query({}, -1, 0.0f, frameIndex, cost, error),
          "query sem banco recusa");

    check(!vendor->build_database({}, error), "banco vazio recusa");

    // Contagem de ossos inconsistente entre poses.
    std::vector<engine::animation::VendorPose> poses{ make_pose(0), make_pose(1) };
    poses[1].bonePositions.pop_back();
    check(!vendor->build_database(poses, error), "contagem inconsistente recusa");

    // Menos de 9 ossos.
    std::vector<engine::animation::VendorPose> small{ make_pose(0) };
    small[0].bonePositions.resize(8 * 3);
    small[0].boneRotations.resize(8 * 4);
    small[0].boneVelocities.resize(8 * 3);
    small[0].boneParents.resize(8);
    check(!vendor->build_database(small, error), "< 9 ossos recusa");

    // Consulta com layout incompatível.
    check(vendor->build_database({ make_pose(0), make_pose(1) }, error),
          "build 2 poses");
    engine::animation::VendorQuery badQuery = make_query(poses, 0);
    badQuery.worldRotations.resize(8 * 4);
    check(!vendor->query(badQuery, -1, 0.0f, frameIndex, cost, error),
          "query com layout incompatível recusa");
    badQuery = make_query(poses, 0);
    check(!vendor->query(badQuery, 5, 0.0f, frameIndex, cost, error),
          "currentFrame fora do banco recusa");
    check(!vendor->query(badQuery, -2, 0.0f, frameIndex, cost, error),
          "currentFrame < -1 recusa");
    check(!vendor->query(badQuery, -1, -1.0f, frameIndex, cost, error),
          "transitionCost negativo recusa");
}

}  // namespace

int main() {
    test_build_and_sizes();
    test_exact_match();
    test_determinism();
    test_transition_cost();
    test_rejections();

    if (failures == 0) {
        std::printf("motion_match_vendor_tests: all checks passed\n");
        return 0;
    }
    std::printf("motion_match_vendor_tests: %d failure(s)\n", failures);
    return 1;
}
