// ProceduralAnimationPipeline.cpp — the only TU implementing the public
// full-body procedural animation pipeline (Agente 4 §10 l.169 "pipeline de
// animação procedural com IK de corpo inteiro, foot placement, look-at, aim e
// constraints"). Deterministic, pure std. Composes the aim through IIkSolver
// and the joint limits through IConstraints; owns the leg/feet/shortest-arc
// orchestration. RegistryJson only for the parser.

#include "engine/animation/IProceduralAnimationPipeline.hpp"

#include "engine/animation/IConstraints.hpp"
#include "engine/animation/IIkSolver.hpp"
#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace engine {
namespace animation {
namespace {

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

bool is_number(const sdk::JsonValue* v) {
    return v != nullptr && v->kind == sdk::JsonValue::Kind::Number;
}

bool is_string(const sdk::JsonValue* v) {
    return v != nullptr && v->kind == sdk::JsonValue::Kind::String;
}

std::string fmt_double(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", value);
    return buf;
}

double clamp(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

AnimVec3 rotate_y(const AnimVec3& v, double yaw) {
    // Rotação em torno de Y (world, Y-up).
    const double c = std::cos(yaw);
    const double s = std::sin(yaw);
    return { v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
}

double length(const AnimVec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

AnimVec3 normalize_or_zero(const AnimVec3& v) {
    const double len = length(v);
    if (len <= 0.0) return { 0, 0, 0 };
    return { v.x / len, v.y / len, v.z / len };
}

// Shortest-arc quaternion: alinha `from` com `to` (ambos normalizados).
// Anti-paralelo → 180° em torno de um eixo perpendicular determinístico.
AnimQuat shortest_arc(const AnimVec3& from, const AnimVec3& to) {
    const AnimVec3 f = normalize_or_zero(from);
    const AnimVec3 t = normalize_or_zero(to);
    double dot = f.x * t.x + f.y * t.y + f.z * t.z;
    if (dot > 1.0) dot = 1.0;
    if (dot < -1.0) dot = -1.0;
    if (dot > 0.999999) return AnimQuat{};
    if (dot < -0.999999) {
        // anti-paralelo: 180° em torno de um eixo perpendicular determinístico
        AnimVec3 axis = AnimVec3::cross(f, AnimVec3{ 0, 1, 0 });
        if (length(axis) < 1e-9) axis = AnimVec3::cross(f, AnimVec3{ 1, 0, 0 });
        axis = normalize_or_zero(axis);
        return { axis.x, axis.y, axis.z, 0.0 };
    }
    const AnimVec3 axis = AnimVec3::cross(f, t);
    const AnimVec3 n = normalize_or_zero(axis);
    const double half = std::acos(dot) * 0.5;
    const double s = std::sin(half);
    return { n.x * s, n.y * s, n.z * s, std::cos(half) };
}

class ProceduralAnimationPipeline final : public IProceduralAnimationPipeline {
public:
    ProceduralAnimationPipeline() = default;

    bool configure(const ProceduralAnimationSpec& spec,
                   std::string& errorOut) override {
        ProceduralAnimationSpec parsed = spec;
        if (!parsed.validate(errorOut)) return false;
        // constraints: registra os limites no adapter irmão
        constraints_ = create_constraints();
        if (!parsed.jointLimits.empty()) {
            if (!constraints_->add_constraint("pipeline", parsed.jointLimits,
                                              errorOut)) {
                return false;
            }
        }
        solver_ = create_ik_solver();
        spec_ = parsed;
        return true;
    }

    ProceduralAnimationResult run(const PipelineBodyState& body,
                                  const IFootTerrainSampler* terrain,
                                  const std::vector<BonePose>& inputPose,
                                  std::string& errorOut) override {
        ProceduralAnimationResult result;
        result.finalPose = inputPose;
        if (!spec_.validate(errorOut)) return result;

        // Mapa bone → índice na pose (ordem preservada).
        std::map<std::string, std::size_t> poseIndex;
        for (std::size_t i = 0; i < inputPose.size(); ++i) {
            if (!poseIndex.emplace(inputPose[i].bone, i).second) {
                errorOut = "pipeline: duplicate bone '" + inputPose[i].bone + "' in pose";
                return result;
            }
        }

        // --- StageLegs + StageFeet + StageIk ---
        StageDiagnostic legsDiag{ PipelineStage::Legs, 0, false, "" };
        StageDiagnostic feetDiag{ PipelineStage::Feet, 0, false, "" };
        StageDiagnostic ikDiag{ PipelineStage::Ik, 0, false, "" };

        for (const PipelineIkChain& chain : spec_.chains) {
            const auto rootIt = poseIndex.find(chain.rootBone);
            const auto midIt = poseIndex.find(chain.midBone);
            const auto endIt = poseIndex.find(chain.endBone);
            if (rootIt == poseIndex.end() || midIt == poseIndex.end() ||
                endIt == poseIndex.end()) {
                errorOut = "pipeline: chain references a bone missing from the pose";
                return result;
            }
            if (chain.kind == IkChainKind::Leg && !spec_.stageLegs &&
                !spec_.stageFeet && !spec_.stageIk) {
                continue;  // todas as etapas de perna desligadas
            }

            // origem world = corpo + offset do quadril rotacionado pelo yaw
            const AnimVec3 origin = body.position + rotate_y(chain.jointOffset, body.yawRadians);
            // alvo de descanso = corpo + restOffset rotacionado
            AnimVec3 target = body.position + rotate_y(chain.restOffset, body.yawRadians);

            // StageLegs: ancoragem no terreno (somente Leg)
            if (spec_.stageLegs && chain.kind == IkChainKind::Leg && terrain != nullptr) {
                const SurfaceSample s = terrain->sample(
                    static_cast<float>(target.x), static_cast<float>(target.z));
                if (s.known) {
                    const double deltaY = static_cast<double>(s.height) - target.y;
                    target.y += deltaY;
                    ++legsDiag.bonesTouched;
                }
            }

            // StageFeet: janela de passo vs a pose de entrada (pé original)
            if (spec_.stageFeet && chain.kind == IkChainKind::Leg) {
                const double poseFootY = inputPose[endIt->second].local.position.y;
                const double dy = target.y - poseFootY;
                if (std::fabs(dy) > spec_.footStepLimit) {
                    const double dir = dy < 0 ? -1.0 : 1.0;
                    target.y = poseFootY + dir * spec_.footStepLimit;
                    feetDiag.stepLimited = true;
                }
                ++feetDiag.bonesTouched;
            }

            // StageIk: shortest-arc do eixo origem→efetor para origem→alvo.
            if (spec_.stageIk) {
                // direção atual do efetor a partir da pose (espaço do corpo
                // rotacionado) — aproximação determinística: usa o offset do
                // rest + posição local do osso do fim.
                const AnimVec3 endLocal = inputPose[endIt->second].local.position;
                AnimVec3 currentDir = origin - (body.position + rotate_y(endLocal, body.yawRadians));
                AnimVec3 targetDir = origin - target;
                const double reach = length(targetDir);
                if (reach < 1e-12) {
                    ikDiag.message = "degenerate chain target";
                } else {
                    currentDir = normalize_or_zero(currentDir);
                    targetDir = normalize_or_zero(targetDir);
                    const AnimQuat q = shortest_arc(currentDir, targetDir);
                    // aplica a rotação nos ossos do meio e do fim (compor à
                    // rotação local existente — o pipeline é um layer)
                    if (q.x != 0.0 || q.y != 0.0 || q.z != 0.0 || q.w != 1.0) {
                        result.finalPose[midIt->second].local.rotation =
                            result.finalPose[midIt->second].local.rotation * q;
                        result.finalPose[endIt->second].local.rotation =
                            result.finalPose[endIt->second].local.rotation * q;
                        ikDiag.bonesTouched += 2;
                    }
                    ResolvedEffector eff;
                    eff.kind = chain.kind;
                    eff.endBone = chain.endBone;
                    eff.targetWorld = target;
                    eff.reachDistance = reach;
                    result.effectors.push_back(eff);
                }
            }
        }
        result.diagnostics.push_back(legsDiag);
        result.diagnostics.push_back(feetDiag);
        result.diagnostics.push_back(ikDiag);

        // --- StageAim (look-at/aim via IIkSolver) ---
        StageDiagnostic aimDiag{ PipelineStage::Aim, 0, false, "" };
        if (spec_.stageAim && body.hasAimTarget && !spec_.aimBones.empty()) {
            const AnimVec3 headDir = normalize_or_zero(body.aimTarget - body.position);
            const AnimVec3 fwd = rotate_y(AnimVec3{ 0, 0, 1 }, body.yawRadians);
            std::string aerr;
            const AnimQuat q = solver_->solve_aim(fwd, headDir, AnimVec3{ 0, 1, 0 }, aerr);
            if (!aerr.empty()) {
                aimDiag.message = aerr;
            } else {
                for (const std::string& boneName : spec_.aimBones) {
                    const auto it = poseIndex.find(boneName);
                    if (it == poseIndex.end()) {
                        errorOut = "pipeline: aim bone missing from the pose";
                        return result;
                    }
                    result.finalPose[it->second].local.rotation =
                        q * result.finalPose[it->second].local.rotation;
                    ++aimDiag.bonesTouched;
                }
            }
        }
        result.diagnostics.push_back(aimDiag);

        // --- StageConstraints (via IConstraints) ---
        StageDiagnostic conDiag{ PipelineStage::Constraints, 0, false, "" };
        if (spec_.stageConstraints && !spec_.jointLimits.empty()) {
            std::string cerr;
            const std::vector<BonePose> clamped =
                constraints_->apply_constraint("pipeline", result.finalPose, cerr);
            if (clamped.empty()) {
                conDiag.message = cerr;
            } else {
                result.finalPose = clamped;
                conDiag.bonesTouched = static_cast<std::uint32_t>(spec_.jointLimits.size());
            }
        }
        result.diagnostics.push_back(conDiag);

        result.ok = true;
        return result;
    }

    std::string serialize_state() const override { return spec_.to_json(); }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        ProceduralAnimationSpec parsed;
        if (!parsed.load_from_json(json, errorOut)) return false;
        return configure(parsed, errorOut);
    }

private:
    ProceduralAnimationSpec spec_;
    std::unique_ptr<IIkSolver> solver_;
    std::unique_ptr<IConstraints> constraints_;
};

}  // namespace

bool ProceduralAnimationSpec::validate(std::string& errorOut) const {
    std::map<std::string, bool> seenBones;
    for (const PipelineIkChain& c : chains) {
        if (c.rootBone.empty() || c.midBone.empty() || c.endBone.empty()) {
            errorOut = "pipeline: chain bones must be non-empty";
            return false;
        }
        if (c.midBone == c.rootBone || c.endBone == c.rootBone ||
            c.endBone == c.midBone) {
            errorOut = "pipeline: chain bones must be distinct";
            return false;
        }
    }
    for (const std::string& b : aimBones) {
        if (b.empty()) {
            errorOut = "pipeline: aim bones must be non-empty";
            return false;
        }
    }
    for (const JointLimit& l : jointLimits) {
        if (l.bone.empty()) {
            errorOut = "pipeline: joint limit bone must be non-empty";
            return false;
        }
        if (l.min_x > l.max_x || l.min_y > l.max_y || l.min_z > l.max_z) {
            errorOut = "pipeline: joint limit min > max";
            return false;
        }
    }
    if (!(footStepLimit > 0.0)) {
        errorOut = "pipeline: footStepLimit must be > 0";
        return false;
    }
    return true;
}

bool ProceduralAnimationSpec::load_from_json(const std::string& json,
                                             std::string& errorOut) {
    sdk::JsonValue root;
    std::string perr;
    if (!sdk::json_parse(json, root, perr) || !root.is_object()) {
        errorOut = "pipeline: " + perr;
        return false;
    }
    ProceduralAnimationSpec parsed = *this;
    if (const sdk::JsonValue* v = root.field("footStepLimit")) {
        if (!is_number(v)) { errorOut = "pipeline: footStepLimit must be a number"; return false; }
        parsed.footStepLimit = v->number;
    }
    if (const sdk::JsonValue* v = root.field("aimBones")) {
        if (v->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "pipeline: aimBones must be an array"; return false;
        }
        parsed.aimBones.clear();
        for (const sdk::JsonValue& el : v->array) {
            if (!el.is_string()) { errorOut = "pipeline: aim bone must be a string"; return false; }
            parsed.aimBones.push_back(el.string);
        }
    }
    if (const sdk::JsonValue* v = root.field("chains")) {
        if (v->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "pipeline: chains must be an array"; return false;
        }
        parsed.chains.clear();
        for (const sdk::JsonValue& el : v->array) {
            if (!el.is_object()) { errorOut = "pipeline: chain must be an object"; return false; }
            PipelineIkChain c;
            const sdk::JsonValue* kind = el.field("kind");
            const sdk::JsonValue* rootB = el.field("rootBone");
            const sdk::JsonValue* midB = el.field("midBone");
            const sdk::JsonValue* endB = el.field("endBone");
            if (!is_string(kind) || !is_string(rootB) || !is_string(midB) ||
                !is_string(endB)) {
                errorOut = "pipeline: malformed chain"; return false;
            }
            c.kind = (kind->string == "arm") ? IkChainKind::Arm : IkChainKind::Leg;
            c.rootBone = rootB->string;
            c.midBone = midB->string;
            c.endBone = endB->string;
            parsed.chains.push_back(c);
        }
    }
    // jointLimits (opcional — formato compacto de objeto)
    if (const sdk::JsonValue* v = root.field("jointLimits")) {
        if (v->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "pipeline: jointLimits must be an array"; return false;
        }
        parsed.jointLimits.clear();
        for (const sdk::JsonValue& el : v->array) {
            if (!el.is_object()) { errorOut = "pipeline: limit must be an object"; return false; }
            const sdk::JsonValue* bn = el.field("bone");
            if (!is_string(bn)) { errorOut = "pipeline: limit bone missing"; return false; }
            JointLimit l;
            l.bone = bn->string;
            parsed.jointLimits.push_back(l);
        }
    }
    if (const sdk::JsonValue* v = root.field("stageLegs")) parsed.stageLegs = v->boolean;
    if (const sdk::JsonValue* v = root.field("stageFeet")) parsed.stageFeet = v->boolean;
    if (const sdk::JsonValue* v = root.field("stageIk")) parsed.stageIk = v->boolean;
    if (const sdk::JsonValue* v = root.field("stageAim")) parsed.stageAim = v->boolean;
    if (const sdk::JsonValue* v = root.field("stageConstraints")) parsed.stageConstraints = v->boolean;
    if (!parsed.validate(errorOut)) return false;
    *this = parsed;
    return true;
}

std::string ProceduralAnimationSpec::to_json() const {
    std::ostringstream os;
    os << "{\"footStepLimit\":" << fmt_double(footStepLimit) << ",\"aimBones\":[";
    bool first = true;
    for (const std::string& b : aimBones) {
        if (!first) os << ',';
        first = false;
        os << '\"' << json_escape(b) << '\"';
    }
    os << "],\"chains\":[";
    first = true;
    for (const PipelineIkChain& c : chains) {
        if (!first) os << ',';
        first = false;
        os << "{\"kind\":\"" << (c.kind == IkChainKind::Arm ? "arm" : "leg")
           << "\",\"rootBone\":\"" << json_escape(c.rootBone) << "\",\"midBone\":\""
           << json_escape(c.midBone) << "\",\"endBone\":\"" << json_escape(c.endBone)
           << "\"}";
    }
    os << "],\"jointLimits\":[";
    first = true;
    for (const JointLimit& l : jointLimits) {
        if (!first) os << ',';
        first = false;
        os << "{\"bone\":\"" << json_escape(l.bone) << "\"}";
    }
    os << "],\"stageLegs\":" << (stageLegs ? "true" : "false")
       << ",\"stageFeet\":" << (stageFeet ? "true" : "false")
       << ",\"stageIk\":" << (stageIk ? "true" : "false")
       << ",\"stageAim\":" << (stageAim ? "true" : "false")
       << ",\"stageConstraints\":" << (stageConstraints ? "true" : "false") << "}";
    return os.str();
}

std::unique_ptr<IProceduralAnimationPipeline>
create_procedural_animation_pipeline() {
    return std::make_unique<ProceduralAnimationPipeline>();
}

}  // namespace animation
}  // namespace engine
