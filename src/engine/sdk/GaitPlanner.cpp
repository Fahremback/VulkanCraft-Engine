// GaitPlanner (FALTANTES §18 item 4): the ONLY TU implementing the public
// IContactPlanner contract. Pure, deterministic locomotion planning —
// `ContactPlanner` + `GaitAsset` + leg-chain assets for arbitrary creatures.
// No external dependency (composição das fachadas públicas — no new vendored
// backend). The planner assumes constant horizontal velocity/yaw over the
// stance period (planted feet stay fixed in world space while the body
// advances); terrain-aware placement is FALTANTES §18 item 5.
#include "engine/animation/IGaitPlanner.hpp"
#include "engine/sdk/RegistryJson.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace engine {
namespace animation {

namespace {

using engine::sdk::JsonValue;
using engine::sdk::json_number;
using engine::sdk::json_parse;
using engine::sdk::json_string;

// Bit-level finiteness guard (the /fp:fast lesson, findings #79):
// std::isfinite(NaN) folds to true under /fp:fast, so the check reads the
// exponent bits directly.
bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

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

// Bit-exact float emission: %.9g round-trips float32 exactly and stays
// deterministic across platforms/locales (the project-wide asset convention).
std::string float_text(float value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return buffer;
}

// A vec3 as a JSON number array.
std::string vec3_text(const glm::vec3& v) {
    return "[" + float_text(v.x) + "," + float_text(v.y) + "," +
           float_text(v.z) + "]";
}

bool parse_vec3(const JsonValue& value, const std::string& what, glm::vec3& out,
                std::string& errorOut) {
    if (!value.is_array() || value.array.size() != 3) {
        errorOut = "gait asset " + what + " must be an array of 3 numbers";
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (value.array[static_cast<std::size_t>(i)].kind !=
            JsonValue::Kind::Number) {
            errorOut = "gait asset " + what + " must be an array of 3 numbers";
            return false;
        }
        out[i] = static_cast<float>(
            value.array[static_cast<std::size_t>(i)].number);
    }
    return true;
}

bool parse_leg(const JsonValue& value, LegChainAsset& out, std::string& errorOut) {
    if (!value.is_object()) {
        errorOut = "gait asset leg must be an object";
        return false;
    }
    out.name = json_string(value, "name", "");
    if (out.name.empty()) {
        errorOut = "gait asset leg name must not be empty";
        return false;
    }
    if (const JsonValue* field = value.field("hipOffset")) {
        if (!parse_vec3(*field, "leg '" + out.name + "' hipOffset", out.hipOffset, errorOut)) {
            return false;
        }
    }
    out.upperLength =
        static_cast<float>(json_number(value, "upperLength", 0.5));
    out.lowerLength =
        static_cast<float>(json_number(value, "lowerLength", 0.5));
    if (const JsonValue* field = value.field("restOffset")) {
        if (!parse_vec3(*field, "leg '" + out.name + "' restOffset", out.restOffset, errorOut)) {
            return false;
        }
    }
    out.maxReach = static_cast<float>(json_number(value, "maxReach", 0.0));
    out.hipBone = static_cast<int>(json_number(value, "hipBone", -1));
    out.kneeBone = static_cast<int>(json_number(value, "kneeBone", -1));
    out.footBone = static_cast<int>(json_number(value, "footBone", -1));
    return true;
}

}  // namespace

bool LegChainAsset::load_from_json(const std::string& jsonText,
                                   std::string& errorOut) {
    JsonValue root;
    if (!json_parse(jsonText, root, errorOut)) {
        errorOut = "gait asset leg: " + errorOut;
        return false;
    }
    LegChainAsset parsed;
    if (!parse_leg(root, parsed, errorOut)) return false;
    std::string err;
    if (!parsed.validate(err)) {
        errorOut = err;
        return false;
    }
    *this = parsed;
    return true;
}

std::string LegChainAsset::to_json() const {
    std::string out = "{\"name\":\"" + json_escape(name) + "\",\"hipOffset\":" +
                      vec3_text(hipOffset) + ",\"upperLength\":" +
                      float_text(upperLength) + ",\"lowerLength\":" +
                      float_text(lowerLength) + ",\"restOffset\":" +
                      vec3_text(restOffset) + ",\"maxReach\":" +
                      float_text(maxReach) + ",\"hipBone\":" +
                      std::to_string(hipBone) + ",\"kneeBone\":" +
                      std::to_string(kneeBone) + ",\"footBone\":" +
                      std::to_string(footBone) + "}";
    return out;
}

bool GaitAsset::load_from_json(const std::string& jsonText,
                               std::string& errorOut) {
    JsonValue root;
    if (!json_parse(jsonText, root, errorOut)) {
        errorOut = "gait asset: " + errorOut;
        return false;
    }
    if (!root.is_object()) {
        errorOut = "gait asset: document must be an object";
        return false;
    }
    const int version = static_cast<int>(json_number(root, "version", 1));
    if (version != 1) {
        errorOut = "gait asset: unsupported version " + std::to_string(version);
        return false;
    }
    GaitAsset parsed;
    parsed.name = json_string(root, "name", "");
    if (parsed.name.empty()) {
        errorOut = "gait asset: name must not be empty";
        return false;
    }
    parsed.cycleDuration =
        static_cast<float>(json_number(root, "cycleDuration", 1.0));
    parsed.stanceFraction =
        static_cast<float>(json_number(root, "stanceFraction", 0.6));
    parsed.stepHeight =
        static_cast<float>(json_number(root, "stepHeight", 0.25));
    parsed.maxStride =
        static_cast<float>(json_number(root, "maxStride", 0.5));
    if (const JsonValue* field = root.field("legPhases")) {
        if (!field->is_array()) {
            errorOut = "gait asset: legPhases must be an array";
            return false;
        }
        parsed.legPhases.clear();
        parsed.legPhases.reserve(field->array.size());
        for (const JsonValue& entry : field->array) {
            if (entry.kind != JsonValue::Kind::Number) {
                errorOut = "gait asset: legPhases entries must be numbers";
                return false;
            }
            parsed.legPhases.push_back(static_cast<float>(entry.number));
        }
    }
    if (const JsonValue* field = root.field("legs")) {
        if (!field->is_array()) {
            errorOut = "gait asset: legs must be an array";
            return false;
        }
        parsed.legs.clear();
        parsed.legs.reserve(field->array.size());
        for (const JsonValue& entry : field->array) {
            LegChainAsset leg;
            if (!parse_leg(entry, leg, errorOut)) return false;
            parsed.legs.push_back(leg);
        }
    }
    std::string err;
    if (!parsed.validate(err)) {
        errorOut = err;
        return false;
    }
    *this = parsed;
    return true;
}

std::string GaitAsset::to_json() const {
    std::string out = "{\"version\":1,\"name\":\"" + json_escape(name) +
                      "\",\"cycleDuration\":" + float_text(cycleDuration) +
                      ",\"stanceFraction\":" + float_text(stanceFraction) +
                      ",\"stepHeight\":" + float_text(stepHeight) +
                      ",\"maxStride\":" + float_text(maxStride) +
                      ",\"legPhases\":[";
    for (std::size_t i = 0; i < legPhases.size(); ++i) {
        if (i > 0) out += ",";
        out += float_text(legPhases[i]);
    }
    out += "],\"legs\":[";
    for (std::size_t i = 0; i < legs.size(); ++i) {
        if (i > 0) out += ",";
        out += legs[i].to_json();
    }
    out += "]}";
    return out;
}

bool LegChainAsset::validate(std::string& errorOut) const {
    if (upperLength <= 0.0f || !std::isfinite(upperLength) ||
        lowerLength <= 0.0f || !std::isfinite(lowerLength)) {
        errorOut = "leg chain '" + name + "' bone lengths must be > 0";
        return false;
    }
    if (!std::isfinite(hipOffset.x) || !std::isfinite(hipOffset.y) ||
        !std::isfinite(hipOffset.z) || !std::isfinite(restOffset.x) ||
        !std::isfinite(restOffset.y) || !std::isfinite(restOffset.z) ||
        !std::isfinite(maxReach) || maxReach < 0.0f) {
        errorOut = "leg chain '" + name +
                   "' offsets must be finite and maxReach >= 0";
        return false;
    }
    if ((hipBone >= 0 && (hipBone == kneeBone || hipBone == footBone)) ||
        (kneeBone >= 0 && kneeBone == footBone)) {
        errorOut = "leg chain '" + name +
                   "' bone indices must be distinct when set";
        return false;
    }
    return true;
}

bool GaitAsset::validate(std::string& errorOut) const {
    if (cycleDuration <= 0.0f || !std::isfinite(cycleDuration)) {
        errorOut = "gait '" + name + "' cycleDuration must be > 0";
        return false;
    }
    if (stanceFraction <= 0.0f || stanceFraction >= 1.0f ||
        !std::isfinite(stanceFraction)) {
        errorOut = "gait '" + name + "' stanceFraction must be in (0, 1)";
        return false;
    }
    if (stepHeight < 0.0f || !std::isfinite(stepHeight)) {
        errorOut = "gait '" + name + "' stepHeight must be >= 0";
        return false;
    }
    if (maxStride <= 0.0f || !std::isfinite(maxStride)) {
        errorOut = "gait '" + name + "' maxStride must be > 0";
        return false;
    }
    if (legs.empty()) {
        errorOut = "gait '" + name + "' must define at least one leg";
        return false;
    }
    if (legPhases.size() != legs.size()) {
        errorOut = "gait '" + name + "' legPhases size (" +
                   std::to_string(legPhases.size()) + ") must match legs (" +
                   std::to_string(legs.size()) + ")";
        return false;
    }
    for (const float phase : legPhases) {
        if (!std::isfinite(phase) || phase < 0.0f || phase >= 1.0f) {
            errorOut = "gait '" + name + "' phase offsets must be in [0, 1)";
            return false;
        }
    }
    for (const LegChainAsset& leg : legs) {
        std::string err;
        if (!leg.validate(err)) {
            errorOut = err;
            return false;
        }
    }
    return true;
}

namespace {

// Rotates a body-space offset into world space for a yaw about +Y.
glm::vec3 rotate_yaw(const glm::vec3& v, float yaw) {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    return glm::vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

class ContactPlannerImpl final : public IContactPlanner {
public:
    bool plan(const GaitAsset& gait, float time, const glm::vec3& bodyPosition,
              float bodyYaw, const glm::vec2& velocity, GaitPlan& outPlan,
              std::string& errorOut) override {
        outPlan.feet.clear();
        std::string err;
        if (!gait.validate(err)) {
            errorOut = err;
            return false;
        }
        if (!std::isfinite(time) || time < 0.0f ||
            !std::isfinite(bodyPosition.x) || !std::isfinite(bodyPosition.y) ||
            !std::isfinite(bodyPosition.z) || !std::isfinite(bodyYaw) ||
            !std::isfinite(velocity.x) || !std::isfinite(velocity.y)) {
            errorOut = "gait plan refused: non-finite input (time, body, yaw "
                       "or velocity)";
            return false;
        }
        const float cycle = gait.cycleDuration;
        const float stanceTime = cycle * gait.stanceFraction;
        const float swingTime = cycle - stanceTime;
        const float speed = glm::length(velocity);
        // Stride per step: the distance the body covers in one cycle, capped
        // at the asset's maxStride. Direction = body velocity (a stationary
        // body strides in place).
        const glm::vec2 strideDir =
            speed > 1e-5f ? velocity / speed : glm::vec2(0.0f);
        const glm::vec2 strideVec = strideDir * std::min(speed * cycle, gait.maxStride);

        outPlan.feet.resize(gait.legs.size());
        for (std::size_t i = 0; i < gait.legs.size(); ++i) {
            const LegChainAsset& leg = gait.legs[i];
            float tau = std::fmod(time / cycle + gait.legPhases[i], 1.0f);
            if (tau < 0.0f) tau += 1.0f;
            const float cycleTime = tau * cycle;
            const bool stance = cycleTime < stanceTime;
            // The foot is planted at the hip's position at the START of the
            // current stance, back-projected by the elapsed stance time
            // (constant-velocity assumption) and the rest offset. It stays
            // fixed in world space for the whole stance.
            const float stanceElapsed = stance ? cycleTime : stanceTime;
            const glm::vec3 planted =
                bodyPosition -
                glm::vec3(velocity.x * stanceElapsed, 0.0f,
                          velocity.y * stanceElapsed) +
                rotate_yaw(leg.restOffset, bodyYaw);
            const glm::vec3 landing =
                planted + glm::vec3(strideVec.x, 0.0f, strideVec.y);

            FootPlan& f = outPlan.feet[i];
            f.legIndex = static_cast<int>(i);
            f.phase = tau;
            f.stance = stance;
            f.landing = landing;
            if (stance) {
                f.targetWorld = planted;
                f.lift = 0.0f;
            } else {
                const float progress =
                    std::min((cycleTime - stanceTime) / swingTime, 1.0f);
                f.targetWorld = glm::mix(planted, landing, progress);
                f.lift = gait.stepHeight * std::sin(glm::pi<float>() * progress);
                f.targetWorld.y += f.lift;
            }
            const glm::vec3 hipWorld =
                bodyPosition + rotate_yaw(leg.hipOffset, bodyYaw);
            const float reach = leg.maxReach > 0.0f
                                    ? leg.maxReach
                                    : leg.upperLength + leg.lowerLength;
            f.withinReach =
                glm::distance(hipWorld, f.targetWorld) <= reach + 1e-3f;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<IContactPlanner> create_contact_planner() {
    return std::make_unique<ContactPlannerImpl>();
}

}  // namespace animation
}  // namespace engine
