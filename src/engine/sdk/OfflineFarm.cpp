// OfflineFarm.cpp
//
// The RuntimeCooker behind IFarmCooker (FALTANTES §18 item 11): validates and
// signs the assets the OFFLINE farm (MuJoCo / MuJoCo MPC / DeepMimic) produced
// — motion clips, small trajectory policies and parameter tables. The training
// tools themselves are NEVER linked into the runtime (reference only, the
// shape-ml / minecraft-spider pattern; DEPENDENCY_POLICY "treinamento, não
// dependência do jogo"); the runtime only ever consumes assets that passed
// this cooker gate.
//
//   cook    — takes the canonical payload for the requested kind, validates it
//             all-or-nothing (exact array sizes, finite samples, increasing
//             times, unique non-empty parameter names), serializes the kind +
//             payload to CANONICAL BYTES (fixed field order, little-endian,
//             exact float bit patterns — never a text re-encode), and signs it
//             with FNV-1a 64. Same payload -> same signature, bit-exact.
//   verify  — recomputes the signature over the payload and compares it to the
//             signed header; also re-checks structural validity for the kind.
//
// The ONLY TU that crosses into the cooker; the public contract is everything
// the caller sees.
#include "engine/farm/IOfflineFarm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <vector>

namespace Engine::Farm {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// Deterministic FNV-1a 64 over a byte span.
std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = kFnvOffset;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(data[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

// Appends the EXACT little-endian bytes of a value/vector to the canonical
// buffer. For floats this is memcpy of the IEEE-754 bits — deterministic
// across machines (no text, no endian-dependent reinterpretation).
void push_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void push_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void push_f32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    push_u32(out, bits);
}

void push_vec3(std::vector<std::uint8_t>& out, const glm::vec3& v) {
    push_f32(out, v.x);
    push_f32(out, v.y);
    push_f32(out, v.z);
}

void push_quat(std::vector<std::uint8_t>& out, const glm::quat& q) {
    push_f32(out, q.x);
    push_f32(out, q.y);
    push_f32(out, q.z);
    push_f32(out, q.w);
}

void push_str(std::vector<std::uint8_t>& out, const std::string& s) {
    push_u32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

bool is_finite_vec3(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool is_finite_quat(const glm::quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
           std::isfinite(q.w);
}

class RuntimeCookerImpl final : public IFarmCooker {
public:
    FarmKind kind() const noexcept override {
        return FarmKind::RuntimeCooker;
    }

    bool cook(CookedAssetKind kind, const ClipPayload& clip,
              const TrajectoryPayload& trajectory,
              const ParameterPayload& parameters, CookedFarmAsset& out,
              std::string& errorOut) override {
        // Exactly ONE payload family per kind (the others must be empty) —
        // a caller mixing families gets a diagnostic, never a silent pick.
        const bool clipSet = !clip.jointRotations.empty() ||
                             !clip.rootPositions.empty() ||
                             !clip.rootOrientations.empty() || clip.jointCount != 0 ||
                             clip.frameCount != 0 || clip.frameRate != 0.0f;
        const bool trajectorySet = !trajectory.times.empty() ||
                                   !trajectory.rootVelocities.empty() ||
                                   !trajectory.turnRates.empty();
        const bool parametersSet = !parameters.names.empty() ||
                                   !parameters.values.empty();
        std::vector<std::uint8_t> bytes;
        bytes.push_back(static_cast<std::uint8_t>(kind));
        switch (kind) {
            case CookedAssetKind::MotionClip:
                if (trajectorySet || parametersSet) {
                    errorOut = "farm cooker: motion clip kind takes ONLY the "
                               "clip payload";
                    return false;
                }
                if (!validate_clip(clip, errorOut)) return false;
                serialize_clip(clip, bytes);
                break;
            case CookedAssetKind::TrajectoryPolicy:
                if (clipSet || parametersSet) {
                    errorOut = "farm cooker: trajectory kind takes ONLY the "
                               "trajectory payload";
                    return false;
                }
                if (!validate_trajectory(trajectory, errorOut)) return false;
                serialize_trajectory(trajectory, bytes);
                break;
            case CookedAssetKind::ParameterTable:
                if (clipSet || trajectorySet) {
                    errorOut = "farm cooker: parameter table kind takes ONLY "
                               "the parameter payload";
                    return false;
                }
                if (!validate_parameters(parameters, errorOut)) return false;
                serialize_parameters(parameters, bytes);
                break;
            default:
                errorOut = "farm cooker: unknown asset kind";
                return false;
        }
        out = CookedFarmAsset{};
        out.kind = kind;
        out.signature = fnv1a64(bytes.data(), bytes.size());
        out.clip = clip;
        out.trajectory = trajectory;
        out.parameters = parameters;
        return true;
    }

    bool verify(const CookedFarmAsset& asset,
                std::string& errorOut) const override {
        std::vector<std::uint8_t> bytes;
        bytes.push_back(static_cast<std::uint8_t>(asset.kind));
        switch (asset.kind) {
            case CookedAssetKind::MotionClip:
                if (!validate_clip(asset.clip, errorOut)) return false;
                serialize_clip(asset.clip, bytes);
                break;
            case CookedAssetKind::TrajectoryPolicy:
                if (!validate_trajectory(asset.trajectory, errorOut)) return false;
                serialize_trajectory(asset.trajectory, bytes);
                break;
            case CookedAssetKind::ParameterTable:
                if (!validate_parameters(asset.parameters, errorOut)) return false;
                serialize_parameters(asset.parameters, bytes);
                break;
            default:
                errorOut = "farm cooker: unknown asset kind";
                return false;
        }
        const std::uint64_t computed = fnv1a64(bytes.data(), bytes.size());
        if (computed != asset.signature) {
            errorOut = "farm cooker: signature mismatch (tampered/corrupt asset)";
            return false;
        }
        return true;
    }

private:
    static bool validate_clip(const ClipPayload& clip, std::string& errorOut) {
        if (clip.jointCount == 0) {
            errorOut = "farm cooker: clip needs at least one joint";
            return false;
        }
        if (clip.frameCount == 0) {
            errorOut = "farm cooker: clip needs at least one frame";
            return false;
        }
        if (!std::isfinite(clip.frameRate) || clip.frameRate <= 0.0f) {
            errorOut = "farm cooker: frameRate must be finite and > 0";
            return false;
        }
        if (clip.rootPositions.size() != clip.frameCount ||
            clip.rootOrientations.size() != clip.frameCount) {
            errorOut = "farm cooker: root arrays must have frameCount entries";
            return false;
        }
        if (clip.jointRotations.size() !=
            clip.jointCount * clip.frameCount) {
            errorOut = "farm cooker: jointRotations must have "
                       "jointCount * frameCount entries";
            return false;
        }
        for (const glm::vec3& p : clip.rootPositions) {
            if (!is_finite_vec3(p)) {
                errorOut = "farm cooker: non-finite root position";
                return false;
            }
        }
        for (const glm::quat& q : clip.rootOrientations) {
            if (!is_finite_quat(q)) {
                errorOut = "farm cooker: non-finite root orientation";
                return false;
            }
        }
        for (const glm::quat& q : clip.jointRotations) {
            if (!is_finite_quat(q)) {
                errorOut = "farm cooker: non-finite joint rotation";
                return false;
            }
        }
        return true;
    }

    static bool validate_trajectory(const TrajectoryPayload& t,
                                    std::string& errorOut) {
        if (t.times.empty()) {
            errorOut = "farm cooker: trajectory needs at least one entry";
            return false;
        }
        if (t.rootVelocities.size() != t.times.size() ||
            t.turnRates.size() != t.times.size()) {
            errorOut = "farm cooker: trajectory arrays must have equal sizes";
            return false;
        }
        for (std::size_t i = 0; i < t.times.size(); ++i) {
            if (!is_finite_vec3(t.times[i])) {
                errorOut = "farm cooker: non-finite trajectory time";
                return false;
            }
            if (i > 0 && t.times[i].x <= t.times[i - 1].x) {
                errorOut = "farm cooker: trajectory times must strictly increase";
                return false;
            }
            if (!is_finite_vec3(t.rootVelocities[i])) {
                errorOut = "farm cooker: non-finite root velocity";
                return false;
            }
            if (!std::isfinite(t.turnRates[i])) {
                errorOut = "farm cooker: non-finite turn rate";
                return false;
            }
        }
        return true;
    }

    static bool validate_parameters(const ParameterPayload& p,
                                    std::string& errorOut) {
        if (p.names.empty()) {
            errorOut = "farm cooker: parameter table needs at least one entry";
            return false;
        }
        if (p.values.size() != p.names.size()) {
            errorOut = "farm cooker: parameter names/values must have equal sizes";
            return false;
        }
        std::set<std::string> seen;
        for (std::size_t i = 0; i < p.names.size(); ++i) {
            if (p.names[i].empty()) {
                errorOut = "farm cooker: parameter name must be non-empty";
                return false;
            }
            if (!seen.insert(p.names[i]).second) {
                errorOut = "farm cooker: duplicate parameter name";
                return false;
            }
            if (!std::isfinite(p.values[i])) {
                errorOut = "farm cooker: non-finite parameter value";
                return false;
            }
        }
        return true;
    }

    static void serialize_clip(const ClipPayload& clip,
                               std::vector<std::uint8_t>& out) {
        push_u64(out, static_cast<std::uint64_t>(clip.jointCount));
        push_u64(out, static_cast<std::uint64_t>(clip.frameCount));
        push_f32(out, clip.frameRate);
        for (const glm::vec3& p : clip.rootPositions) push_vec3(out, p);
        for (const glm::quat& q : clip.rootOrientations) push_quat(out, q);
        for (const glm::quat& q : clip.jointRotations) push_quat(out, q);
    }

    static void serialize_trajectory(const TrajectoryPayload& t,
                                     std::vector<std::uint8_t>& out) {
        push_u64(out, static_cast<std::uint64_t>(t.times.size()));
        for (std::size_t i = 0; i < t.times.size(); ++i) {
            push_vec3(out, t.times[i]);
            push_vec3(out, t.rootVelocities[i]);
            push_f32(out, t.turnRates[i]);
        }
    }

    static void serialize_parameters(const ParameterPayload& p,
                                     std::vector<std::uint8_t>& out) {
        push_u64(out, static_cast<std::uint64_t>(p.names.size()));
        for (std::size_t i = 0; i < p.names.size(); ++i) {
            push_str(out, p.names[i]);
            push_f32(out, p.values[i]);
        }
    }
};

}  // namespace

std::unique_ptr<IFarmCooker> create_farm_cooker(FarmKind kind,
                                                std::string& errorOut) {
    if (kind == FarmKind::RuntimeCooker) {
        return std::make_unique<RuntimeCookerImpl>();
    }
    // MuJoCo / MuJoCo MPC / DeepMimic are OFFLINE farm tools (training,
    // validation) — reference only, never linked into the runtime
    // (DEPENDENCY_POLICY). Refuse with a diagnostic: a missing training
    // backend must never look like a working runtime simulator (the
    // deformable/FEMFX/Tressfx plugin pattern).
    const char* name = "unknown";
    if (kind == FarmKind::Mujoco) name = "MuJoCo";
    if (kind == FarmKind::MujocoMpc) name = "MuJoCo MPC";
    if (kind == FarmKind::DeepMimic) name = "DeepMimic";
    errorOut = std::string("farm cooker: ") + name +
               " is an OFFLINE training/validation tool — reference only, "
               "never linked into the runtime (DEPENDENCY_POLICY); assets "
               "produced by the farm are consumed via the RuntimeCooker gate";
    return nullptr;
}

}  // namespace Engine::Farm
