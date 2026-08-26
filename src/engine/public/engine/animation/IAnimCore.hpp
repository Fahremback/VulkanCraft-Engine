#pragma once
// IAnimCore — contrato público do NÚCLEO de animação (skeleton/clip/blend)
// (agente 4 §4 item 1).
//
// Hierarquia de ossos, clips de animação e blend tree 1D determinísticos e
// headless: SEM GPU, SEM RNG, SEM relógio de parede, SEM estado global — as
// mesmas specs + tempo produzem as mesmas poses bit-exatas entre instâncias.
// JSON versionado all-or-nothing bit-exact. Este é o CORE de dados/amostragem
// que os sistemas de alto nível (state machine, motion matching, IK) usam; a
// integração com o runtime de render (ozz etc.) fica fora do contrato.
//
// Modelo:
//   - skeleton: ossos com id + parent (índice; -1 = raiz) + bind local.
//   - clip: trilha por osso com keyframes (tempo + transform); amostragem
//     determinística (lerp p/ posição/escala, slerp p/ rotação, clamp nas
//     pontas). Ossos sem trilha usam o bind.
//   - blend: mistura 1D de DOIS clips (mesmo skeleton) por parâmetro;
//     weight = (param − param_min)/(param_max − param_min) clampado;
//     cada clip é amostrado no tempo normalizado t·duration.
//   - local_to_world: composição hierárquica determinística (posição do filho
//     rotacionada/escalada pelo mundo do pai).
//
// Escopo §4 item 1: skeleton/clip/blend CORE entregues; state machine,
// additive, masks, events e root motion = unidades futuras (registradas).

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

struct AnimVec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    AnimVec3 operator+(const AnimVec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    AnimVec3 operator-(const AnimVec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    AnimVec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    AnimVec3 operator*(const AnimVec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    static AnimVec3 lerp(const AnimVec3& a, const AnimVec3& b, double t) {
        return a + (b - a) * t;
    }
    static AnimVec3 cross(const AnimVec3& a, const AnimVec3& b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
    }
};

struct AnimQuat {
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;  // identidade por default
    double dot(const AnimQuat& o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }
    AnimQuat normalized() const {
        const double len = std::sqrt(dot(*this));
        if (len <= 0.0) return AnimQuat{};
        return {x / len, y / len, z / len, w / len};
    }
    AnimQuat inverse() const { return {-x, -y, -z, w}; }  // conjugado (unitário)
    AnimQuat operator*(const AnimQuat& o) const {  // Hamilton (this ∘ o)
        return {w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w,
                w * o.w - x * o.x - y * o.y - z * o.z};
    }
    // Slerp determinístico (caminho mais curto; angulo ~0 → a).
    static AnimQuat slerp(AnimQuat a, AnimQuat b, double t) {
        a = a.normalized();
        b = b.normalized();
        double d = a.dot(b);
        if (d < 0.0) {
            b = {-b.x, -b.y, -b.z, -b.w};
            d = -d;
        }
        if (d > 1.0) d = 1.0;
        const double angle = std::acos(d);
        if (std::fabs(angle) < 1e-12) return a;
        const double s = std::sin(angle);
        const double wa = std::sin((1.0 - t) * angle) / s;
        const double wb = std::sin(t * angle) / s;
        return AnimQuat{a.x * wa + b.x * wb, a.y * wa + b.y * wb,
                        a.z * wa + b.z * wb, a.w * wa + b.w * wb}
            .normalized();
    }
    // v' = v + 2w(c×v) + 2(c×(c×v)), c = (x,y,z)
    AnimVec3 rotate(const AnimVec3& v) const {
        const AnimVec3 cv{x, y, z};
        const AnimVec3 c1 = AnimVec3::cross(cv, v);
        const AnimVec3 c2 = AnimVec3::cross(cv, c1);
        return v + c1 * (2.0 * w) + c2 * 2.0;
    }
};

struct AnimTransform {
    AnimVec3 position;
    AnimQuat rotation;
    AnimVec3 scale{1.0, 1.0, 1.0};
    static AnimTransform lerp(const AnimTransform& a, const AnimTransform& b,
                              double t) {
        return {AnimVec3::lerp(a.position, b.position, t),
                AnimQuat::slerp(a.rotation, b.rotation, t),
                AnimVec3::lerp(a.scale, b.scale, t)};
    }
};

struct Bone {
    std::string id;
    int parent = -1;  // -1 = raiz
    AnimTransform bind_local;
};

struct SkeletonSpec {
    std::string id;
    std::vector<Bone> bones;
    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

struct Keyframe {
    double t = 0.0;
    AnimTransform value;
};

struct BoneTrack {
    std::string bone;
    std::vector<Keyframe> keys;  // tempos estritamente crescentes, em [0, duration]
};

struct ClipSpec {
    std::string id;
    std::string skeleton;
    double duration = 1.0;  // > 0
    std::vector<BoneTrack> tracks;
    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;
};

struct BlendSpec {
    std::string id;
    std::string clip_a;
    std::string clip_b;
    double param_min = 0.0;
    double param_max = 1.0;  // > param_min
    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;
};

struct BonePose {
    std::string bone;
    AnimTransform local;
};

struct WorldPose {
    std::string bone;
    AnimTransform world;
};

// Núcleo de animação (registro + amostragem determinística).
class IAnimCore {
public:
    virtual ~IAnimCore() = default;

    virtual bool add_skeleton(const SkeletonSpec& spec, std::string& errorOut) = 0;
    // Refere uma skeleton já registrada (recusa all-or-nothing).
    virtual bool add_clip(const ClipSpec& spec, std::string& errorOut) = 0;
    // Refere dois clips já registrados do MESMO skeleton (recusa all-or-nothing).
    virtual bool add_blend(const BlendSpec& spec, std::string& errorOut) = 0;

    virtual bool has_skeleton(const std::string& id) const = 0;
    virtual bool has_clip(const std::string& id) const = 0;
    virtual bool has_blend(const std::string& id) const = 0;

    // Duração do clip em segundos (0.0 + erro honesto p/ id desconhecido).
    virtual double clip_duration(const std::string& clipId,
                                 std::string& errorOut) = 0;

    // Pose local de repouso (bind) de TODOS os ossos, na ordem de declaração
    // da skeleton (erro honesto p/ skeleton desconhecida).
    virtual std::vector<BonePose> bind_pose(const std::string& skeletonId,
                                            std::string& errorOut) = 0;

    virtual std::vector<std::string> skeleton_ids() const = 0;
    virtual std::vector<std::string> clip_ids() const = 0;
    virtual std::vector<std::string> blend_ids() const = 0;

    // Amostra o clip no tempo `t` (clamp em [0, duration]); pose na ordem de
    // declaração da skeleton; osso sem trilha = bind local.
    virtual std::vector<BonePose> sample_clip(const std::string& clipId, double t,
                                              std::string& errorOut) = 0;

    // Mistura 1D dos dois clips em `t` ∈ [0,1] normalizado por parâmetro.
    virtual std::vector<BonePose> sample_blend(const std::string& blendId,
                                               double param, double t,
                                               std::string& errorOut) = 0;

    // Compõe a hierarquia: mundo do filho = pai.mundo.rot·(local·pai.scale) +
    // pai.mundo.pos; rotação = pai.mundo.rot·local.rot; escala multiplicada.
    virtual std::vector<WorldPose> local_to_world(
        const std::string& skeletonId, const std::vector<BonePose>& poses,
        std::string& errorOut) = 0;

    // Registro completo serializado bit-exact / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IAnimCore).
std::unique_ptr<IAnimCore> create_anim_core();

}  // namespace engine::animation
