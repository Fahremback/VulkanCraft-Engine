// IkSolver.cpp — adapter único de IIkSolver (engine::animation).
// 2-bone analytic (triângulo L1/L2/d com dobra no plano de bend_dir) +
// look-at em dois passos (shortest-arc + correção de roll). Sem estado.

#include "engine/animation/IIkSolver.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <cmath>

namespace engine::animation {
namespace {

constexpr double kEps = 1e-12;

double vlen(const AnimVec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

double vdot(const AnimVec3& a, const AnimVec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Normaliza ou devolve false p/ vetor (quase) nulo.
bool vnorm(const AnimVec3& v, AnimVec3& out) {
    const double len = vlen(v);
    if (len <= kEps) return false;
    out = {v.x / len, v.y / len, v.z / len};
    return true;
}

// Quat de rotação em torno de um eixo (normalizado) por um ângulo.
AnimQuat quat_axis_angle(const AnimVec3& axis, double angle) {
    const double half = angle * 0.5;
    const double s = std::sin(half);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

// Eixo perpendicular determinístico a `dir` (fallback p/ degenerados).
AnimVec3 perp_fallback(const AnimVec3& dir) {
    AnimVec3 up{0.0, 1.0, 0.0};
    AnimVec3 c = AnimVec3::cross(dir, up);
    if (vlen(c) > kEps) return c;
    // dir ≈ paralelo a Y → usa X como referência.
    return AnimVec3::cross(dir, AnimVec3{1.0, 0.0, 0.0});
}

class IkSolver final : public IIkSolver {
public:
    TwoBoneResult solve_two_bone(const AnimVec3& origin,
                                 const AnimVec3& target, double length_a,
                                 double length_b, const AnimVec3& bend_dir,
                                 std::string& errorOut) const override {
        TwoBoneResult out;
        if (!std::isfinite(length_a) || !std::isfinite(length_b) ||
            length_a <= 0.0 || length_b <= 0.0) {
            errorOut = "segment lengths must be finite and > 0";
            return out;
        }
        AnimVec3 dir;
        const AnimVec3 dvec{target.x - origin.x, target.y - origin.y,
                            target.z - origin.z};
        if (!vnorm(dvec, dir)) {
            errorOut = "target coincides with origin (degenerate)";
            return out;
        }
        double d = vlen(dvec);
        const double max_reach = length_a + length_b;
        const double min_reach = std::fabs(length_a - length_b);
        if (d > max_reach || d < min_reach) {
            out.stretched = true;
            d = std::max(min_reach, std::min(d, max_reach));
        }
        if (d <= kEps) {
            errorOut = "target coincides with origin (degenerate)";
            return out;
        }

        // Triângulo: ângulo no ombro (entre O→T e o primeiro segmento) e
        // ângulo no cotovelo (entre os dois segmentos).
        double cos_e =
            (length_a * length_a + d * d - length_b * length_b) /
            (2.0 * length_a * d);
        cos_e = std::max(-1.0, std::min(1.0, cos_e));
        out.elbow_angle = std::acos(cos_e);
        double cos_j =
            (length_a * length_a + length_b * length_b - d * d) /
            (2.0 * length_a * length_b);
        cos_j = std::max(-1.0, std::min(1.0, cos_j));
        out.joint_angle = std::acos(cos_j);

        // Plano de dobra: projeção perpendicular de bend_dir em relação a dir.
        AnimVec3 bend;
        if (!vnorm(bend_dir, bend)) {
            errorOut = "bend_dir must be non-zero";
            return out;
        }
        AnimVec3 n;
        const double along = vdot(bend, dir);
        const AnimVec3 proj{bend.x - dir.x * along, bend.y - dir.y * along,
                            bend.z - dir.z * along};
        if (!vnorm(proj, n)) {
            n = perp_fallback(dir);
        }
        const double sin_e = std::sin(out.elbow_angle);
        out.elbow_pos = {
            origin.x + length_a * (cos_e * dir.x + sin_e * n.x),
            origin.y + length_a * (cos_e * dir.y + sin_e * n.y),
            origin.z + length_a * (cos_e * dir.z + sin_e * n.z),
        };
        // Effector: estica o segundo segmento do cotovelo ao alvo (ou além,
        // se esticado — igual ao alvo real quando alcançável).
        AnimVec3 to_effector{target.x - out.elbow_pos.x,
                             target.y - out.elbow_pos.y,
                             target.z - out.elbow_pos.z};
        AnimVec3 e_dir;
        if (!vnorm(to_effector, e_dir)) {
            errorOut = "degenerate elbow geometry";
            return out;
        }
        const double e_len = std::min(vlen(to_effector), length_b);
        out.effector = {out.elbow_pos.x + e_dir.x * e_len,
                        out.elbow_pos.y + e_dir.y * e_len,
                        out.elbow_pos.z + e_dir.z * e_len};
        errorOut.clear();
        return out;
    }

    AnimQuat solve_aim(const AnimVec3& axis, const AnimVec3& target_dir,
                       const AnimVec3& up, std::string& errorOut) const override {
        AnimVec3 a, t, u;
        if (!vnorm(axis, a) || !vnorm(target_dir, t) || !vnorm(up, u)) {
            errorOut = "axis/target_dir/up must be non-zero";
            return AnimQuat{};
        }
        double dot = std::max(-1.0, std::min(1.0, vdot(a, t)));
        if (dot > 1.0 - 1e-9) {
            errorOut.clear();
            return AnimQuat{};  // já alinhado
        }
        if (dot < -1.0 + 1e-9) {
            // Anti-paralelo: 180° em torno de um eixo perpendicular
            // determinístico (ao plano de a e up).
            AnimVec3 perp;
            if (!vnorm(AnimVec3::cross(a, u), perp)) {
                perp = perp_fallback(a);
            }
            errorOut.clear();
            return quat_axis_angle(perp, 3.14159265358979323846);
        }
        // Passo 1: shortest-arc a → t.
        AnimVec3 rot_axis;
        if (!vnorm(AnimVec3::cross(a, t), rot_axis)) {
            errorOut = "degenerate aim axis";
            return AnimQuat{};
        }
        const AnimQuat q1 = quat_axis_angle(rot_axis, std::acos(dot));
        // Passo 2: correção de roll — aproxima o up desejado em torno de t.
        AnimVec3 desired;
        const double along = vdot(u, t);
        const AnimVec3 up_perp{u.x - t.x * along, u.y - t.y * along,
                               u.z - t.z * along};
        if (vnorm(up_perp, desired)) {
            const AnimVec3 v = q1.rotate(u);
            const double valong = vdot(v, t);
            const AnimVec3 v_perp{v.x - t.x * valong, v.y - t.y * valong,
                                  v.z - t.z * valong};
            AnimVec3 w;
            if (vnorm(v_perp, w)) {
                // Ângulo entre w e desired em torno de t (sinal via produto
                // misto com o eixo de rotação).
                const AnimVec3 c = AnimVec3::cross(w, desired);
                double angle = std::acos(
                    std::max(-1.0, std::min(1.0, vdot(w, desired))));
                if (vdot(c, t) < 0.0) angle = -angle;
                const AnimQuat q2 = quat_axis_angle(t, angle);
                errorOut.clear();
                return (q2 * q1).normalized();
            }
        }
        errorOut.clear();
        return q1;
    }

    std::string serialize_state() const override { return "{}"; }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) return false;
        if (!doc.is_object()) {
            errorOut = "ik solver state must be an object";
            return false;
        }
        errorOut.clear();
        return true;
    }
};

}  // namespace

std::unique_ptr<IIkSolver> create_ik_solver() {
    return std::unique_ptr<IIkSolver>(new IkSolver());
}

}  // namespace engine::animation
