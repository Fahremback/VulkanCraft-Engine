// Skinning.cpp — adapter único de ISkinning (engine::animation).
// skin[i] = world[i] · bind[i]⁻¹ via IAnimCore (local_to_world + bind_pose);
// deformação por 4 influências com pesos normalizados. Sem estado.

#include "engine/animation/ISkinning.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <cmath>
#include <map>

namespace engine::animation {
namespace {

// Matriz 4x4 row-major a partir de (T, R, S): M = T·R·S.
void mat4_from_transform(const AnimTransform& t, double out[16]) {
    const AnimQuat& q = t.rotation;
    const double r00 = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
    const double r01 = 2.0 * (q.x * q.y - q.w * q.z);
    const double r02 = 2.0 * (q.x * q.z + q.w * q.y);
    const double r10 = 2.0 * (q.x * q.y + q.w * q.z);
    const double r11 = 1.0 - 2.0 * (q.x * q.x + q.z * q.z);
    const double r12 = 2.0 * (q.y * q.z - q.w * q.x);
    const double r20 = 2.0 * (q.x * q.z - q.w * q.y);
    const double r21 = 2.0 * (q.y * q.z + q.w * q.x);
    const double r22 = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);
    out[0] = r00 * t.scale.x; out[1] = r01 * t.scale.y;
    out[2] = r02 * t.scale.z; out[3] = t.position.x;
    out[4] = r10 * t.scale.x; out[5] = r11 * t.scale.y;
    out[6] = r12 * t.scale.z; out[7] = t.position.y;
    out[8] = r20 * t.scale.x; out[9] = r21 * t.scale.y;
    out[10] = r22 * t.scale.z; out[11] = t.position.z;
    out[12] = 0.0; out[13] = 0.0; out[14] = 0.0; out[15] = 1.0;
}

void mat4_mul(const double a[16], const double b[16], double out[16]) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a[row * 4 + k] * b[k * 4 + col];
            }
            out[row * 4 + col] = sum;
        }
    }
}

// Inversa afim de (T, R, S): M⁻¹ = [[S⁻¹·Rᵀ, −S⁻¹·Rᵀ·t], [0, 1]].
void mat4_inverse_affine(const double m[16], double out[16]) {
    // L[i][j] = Rᵀ[i][j] / s[j]  →  L[i][j] = R[j][i] / s[j], com s = coluna
    // de escala de M (r00*r01... na verdade s_j = |coluna j da parte linear|;
    // como M = T·R·S, a escala de cada coluna é exatamente s[j]).
    const double sx = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    const double sy = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    const double sz = std::sqrt(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
    const double s[3] = {sx, sy, sz};
    double L[9];  // S⁻¹·Rᵀ: L[i][j] = R[j][i]/s[j] = m[j*4+i]/(s[i]·s[j])
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const double si = s[i] > 0.0 ? s[i] : 1.0;
            const double sj = s[j] > 0.0 ? s[j] : 1.0;
            L[i * 3 + j] = m[j * 4 + i] / (si * sj);
        }
    }
    // t' = −L·t
    const double tx = m[3], ty = m[7], tz = m[11];
    out[0] = L[0]; out[1] = L[1]; out[2] = L[2];
    out[3] = -(L[0] * tx + L[1] * ty + L[2] * tz);
    out[4] = L[3]; out[5] = L[4]; out[6] = L[5];
    out[7] = -(L[3] * tx + L[4] * ty + L[5] * tz);
    out[8] = L[6]; out[9] = L[7]; out[10] = L[8];
    out[11] = -(L[6] * tx + L[7] * ty + L[8] * tz);
    out[12] = 0.0; out[13] = 0.0; out[14] = 0.0; out[15] = 1.0;
}

class Skinning final : public ISkinning {
public:
    explicit Skinning(IAnimCore& core) : core_(core) {}

    std::vector<SkinMatrix> skin_matrices(
        const std::string& skeletonId, const std::vector<BonePose>& pose,
        std::string& errorOut) const override {
        std::vector<SkinMatrix> out;
        if (!core_.has_skeleton(skeletonId)) {
            errorOut = "unknown skeleton \"" + skeletonId + "\"";
            return out;
        }
        const std::vector<BonePose> bind =
            core_.bind_pose(skeletonId, errorOut);
        if (!errorOut.empty()) return out;
        if (pose.size() != bind.size()) {
            errorOut = "pose must cover every bone of the skeleton";
            return out;
        }
        for (std::size_t i = 0; i < pose.size(); ++i) {
            if (pose[i].bone != bind[i].bone) {
                errorOut = "pose bone \"" + pose[i].bone +
                           "\" does not match skeleton order (\"" +
                           bind[i].bone + "\" expected)";
                return out;
            }
        }
        const std::vector<WorldPose> world =
            core_.local_to_world(skeletonId, pose, errorOut);
        if (!errorOut.empty()) return out;
        if (world.size() != bind.size()) {
            errorOut = "world pose size mismatch";
            return out;
        }
        out.resize(bind.size());
        for (std::size_t i = 0; i < bind.size(); ++i) {
            if (world[i].bone != bind[i].bone) {
                errorOut = "world pose order mismatch at bone \"" +
                           world[i].bone + "\"";
                return out;
            }
            double wm[16], bm[16], binv[16], skin[16];
            mat4_from_transform(world[i].world, wm);
            mat4_from_transform(bind[i].local, bm);
            mat4_inverse_affine(bm, binv);
            mat4_mul(wm, binv, skin);
            for (int k = 0; k < 16; ++k) out[i].m[k] = skin[k];
        }
        errorOut.clear();
        return out;
    }

    AnimVec3 apply_skin(const std::vector<SkinMatrix>& skin,
                        const SkinVertex& v,
                        std::string& errorOut) const override {
        const int bones[4] = {v.bone0, v.bone1, v.bone2, v.bone3};
        const double weights[4] = {v.weight0, v.weight1, v.weight2,
                                   v.weight3};
        double total = 0.0;
        for (int i = 0; i < 4; ++i) {
            if (bones[i] < 0) continue;
            if (bones[i] >= static_cast<int>(skin.size())) {
                errorOut = "bone index " + std::to_string(bones[i]) +
                           " out of range";
                return {};
            }
            if (!std::isfinite(weights[i]) || weights[i] < 0.0) {
                errorOut = "weights must be finite and >= 0";
                return {};
            }
            total += weights[i];
        }
        if (total <= 0.0) {
            errorOut = "vertex has no influence (zero total weight)";
            return {};
        }
        AnimVec3 out{0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            if (bones[i] < 0) continue;
            const double w = weights[i] / total;
            const AnimVec3 p = skin[static_cast<std::size_t>(bones[i])]
                                   .apply(v.position);
            out = {out.x + p.x * w, out.y + p.y * w, out.z + p.z * w};
        }
        errorOut.clear();
        return out;
    }

    std::vector<AnimVec3> skin_vertices(
        const std::string& skeletonId, const std::vector<BonePose>& pose,
        const std::vector<SkinVertex>& vertices,
        std::string& errorOut) const override {
        const std::vector<SkinMatrix> skin =
            skin_matrices(skeletonId, pose, errorOut);
        if (!errorOut.empty()) return {};
        std::vector<AnimVec3> out;
        out.reserve(vertices.size());
        for (const SkinVertex& v : vertices) {
            const AnimVec3 p = apply_skin(skin, v, errorOut);
            if (!errorOut.empty()) return {};
            out.push_back(p);
        }
        errorOut.clear();
        return out;
    }

    std::string serialize_state() const override { return "{}"; }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) return false;
        if (!doc.is_object()) {
            errorOut = "skinning state must be an object";
            return false;
        }
        errorOut.clear();
        return true;
    }

private:
    IAnimCore& core_;
};

}  // namespace

std::unique_ptr<ISkinning> create_skinning(IAnimCore& core) {
    return std::unique_ptr<ISkinning>(new Skinning(core));
}

}  // namespace engine::animation
