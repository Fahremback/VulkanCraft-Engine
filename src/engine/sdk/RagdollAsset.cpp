// RagdollAsset.cpp — the only TU implementing the public ragdoll asset
// contract (Agente 4 §4 item 55 CORE): a configurable ragdoll skeleton
// (joints / swing-twist limits / position drives / auto-balance / blend)
// serialized as versioned JSON. Load is all-or-nothing (refuses malformed
// documents with a diagnostic, never clamps); the emitter round-trips
// float32 exactly (%.9g), so to_json() -> load_from_json() is stable.
// build_bones() maps the asset onto the public runtime skeleton (RagdollBone)
// — the wiring point for IRagdoll::create_ragdoll / ActiveRagdoll.
//
// Numeric validation uses BIT-LEVEL finite checks: the project compiles with
// /fp:fast (findings #79), which folds std::isfinite(NaN) to true.

#include "engine/gameplay/IRagdollAsset.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>
#include <unordered_set>

namespace engine {
namespace gameplay {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    // IEEE-754: exponent all-ones => NaN or infinity.
    return (bits & 0x7f800000u) != 0x7f800000u;
}

bool finite_vec3(const glm::vec3& value) {
    return finite_float(value.x) && finite_float(value.y) && finite_float(value.z);
}

bool finite_quat(const glm::quat& value) {
    return finite_float(value.x) && finite_float(value.y) &&
           finite_float(value.z) && finite_float(value.w);
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

std::string emit_float(float value) {
    std::ostringstream out;
    out << std::setprecision(9) << value;
    return out.str();
}

std::string emit_vec3(const glm::vec3& v) {
    return "[" + emit_float(v.x) + "," + emit_float(v.y) + "," +
           emit_float(v.z) + "]";
}

std::string emit_quat(const glm::quat& q) {
    return "[" + emit_float(q.x) + "," + emit_float(q.y) + "," +
           emit_float(q.z) + "," + emit_float(q.w) + "]";
}

bool read_vec3(const sdk::JsonValue& object, const std::string& key,
               glm::vec3& out) {
    const sdk::JsonValue* value = object.field(key);
    if (value == nullptr || value->kind != sdk::JsonValue::Kind::Array ||
        value->array.size() != 3) {
        return false;
    }
    glm::vec3 result{0.0f};
    for (int i = 0; i < 3; ++i) {
        if (value->array[i].kind != sdk::JsonValue::Kind::Number) return false;
        result[i] = static_cast<float>(value->array[i].number);
    }
    if (!finite_vec3(result)) return false;
    out = result;
    return true;
}

bool read_quat(const sdk::JsonValue& object, const std::string& key,
               glm::quat& out) {
    const sdk::JsonValue* value = object.field(key);
    if (value == nullptr || value->kind != sdk::JsonValue::Kind::Array ||
        value->array.size() != 4) {
        return false;
    }
    glm::quat result{1.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; ++i) {
        if (value->array[i].kind != sdk::JsonValue::Kind::Number) return false;
        result[i] = static_cast<float>(value->array[i].number);
    }
    if (!finite_quat(result)) return false;
    out = result;
    return true;
}

// Validação estrutural/semântica compartilhada por load e validate.
bool check_asset(const RagdollAsset& asset, std::string& errorOut) {
    if (asset.version != 1) {
        errorOut = "ragdoll asset: unsupported version " +
                   std::to_string(asset.version);
        return false;
    }
    if (asset.name.empty()) {
        errorOut = "ragdoll asset: name must be non-empty";
        return false;
    }
    if (asset.joints.empty()) {
        errorOut = "ragdoll asset: joints must be non-empty";
        return false;
    }
    if (!finite_float(asset.blend.recoverRate) || asset.blend.recoverRate < 0.0f) {
        errorOut = "ragdoll asset: blend.recoverRate must be finite and >= 0";
        return false;
    }

    std::unordered_set<std::string> names;
    std::map<std::string, std::size_t> indexByName;
    for (std::size_t i = 0; i < asset.joints.size(); ++i) {
        const RagdollJoint& joint = asset.joints[i];
        if (joint.name.empty()) {
            errorOut = "ragdoll asset: joint at index " + std::to_string(i) +
                       " has an empty name";
            return false;
        }
        if (!names.insert(joint.name).second) {
            errorOut = "ragdoll asset: duplicate joint name '" + joint.name + "'";
            return false;
        }
        indexByName[joint.name] = i;

        if (!finite_vec3(joint.anchor) || !finite_quat(joint.rotation)) {
            errorOut = "ragdoll asset: joint '" + joint.name +
                       "' has non-finite anchor/rotation";
            return false;
        }
        if (!finite_float(joint.length) || joint.length <= 0.0f ||
            !finite_float(joint.radius) || joint.radius <= 0.0f ||
            !finite_float(joint.mass) || joint.mass <= 0.0f) {
            errorOut = "ragdoll asset: joint '" + joint.name +
                       "' needs length/radius/mass > 0 (finite)";
            return false;
        }
        if (!finite_float(joint.limits.swingLimitX) ||
            !finite_float(joint.limits.swingLimitY) ||
            !finite_float(joint.limits.twistLimit) ||
            joint.limits.swingLimitX < 0.0f || joint.limits.swingLimitY < 0.0f ||
            joint.limits.twistLimit < 0.0f) {
            errorOut = "ragdoll asset: joint '" + joint.name +
                       "' limits must be finite and >= 0";
            return false;
        }
        if (joint.drive.enabled) {
            if (!finite_float(joint.drive.frequency) ||
                joint.drive.frequency <= 0.0f ||
                !finite_float(joint.drive.damping) || joint.drive.damping < 0.0f) {
                errorOut = "ragdoll asset: joint '" + joint.name +
                           "' drive needs frequency > 0 and damping >= 0";
                return false;
            }
        }
    }

    // Parents conhecidos + sem ciclos (caminho termina numa raiz "").
    std::vector<char> state(asset.joints.size(), 0);  // 0=novo 1=em caminho 2=ok
    for (std::size_t i = 0; i < asset.joints.size(); ++i) {
        std::size_t cursor = i;
        while (!asset.joints[cursor].parent.empty()) {
            const auto found = indexByName.find(asset.joints[cursor].parent);
            if (found == indexByName.end()) {
                errorOut = "ragdoll asset: joint '" + asset.joints[cursor].name +
                           "' has unknown parent '" + asset.joints[cursor].parent +
                           "'";
                return false;
            }
            if (state[cursor] == 1) {
                errorOut = "ragdoll asset: joint '" + asset.joints[cursor].name +
                           "' participates in a parent cycle";
                return false;
            }
            if (state[cursor] == 2) break;  // já resolvido
            state[cursor] = 1;
            cursor = found->second;
        }
        // Marca o caminho inteiro como resolvido.
        cursor = i;
        while (state[cursor] != 2) {
            state[cursor] = 2;
            if (asset.joints[cursor].parent.empty()) break;
            cursor = indexByName[asset.joints[cursor].parent];
        }
    }
    return true;
}

}  // namespace

bool RagdollAsset::load_from_json(const std::string& jsonText,
                                  std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
        if (errorOut.empty()) errorOut = "ragdoll asset: root must be an object";
        return false;
    }

    RagdollAsset parsed;
    parsed.name = sdk::json_string(root, "name", "");
    parsed.version = static_cast<int>(sdk::json_number(root, "version", 1));
    parsed.autoBalance = sdk::json_bool(root, "autoBalance", false);

    const sdk::JsonValue* blendValue = root.field("blend");
    if (blendValue != nullptr) {
        if (!blendValue->is_object()) {
            errorOut = "ragdoll asset: blend must be an object";
            return false;
        }
        parsed.blend.recoverRate =
            static_cast<float>(sdk::json_number(*blendValue, "recoverRate", 1.0));
    }

    const sdk::JsonValue* jointsValue = root.field("joints");
    if (jointsValue == nullptr || jointsValue->kind != sdk::JsonValue::Kind::Array) {
        errorOut = "ragdoll asset: joints must be an array";
        return false;
    }
    parsed.joints.reserve(jointsValue->array.size());
    for (const sdk::JsonValue& jointValue : jointsValue->array) {
        if (!jointValue.is_object()) {
            errorOut = "ragdoll asset: each joint must be an object";
            return false;
        }
        RagdollJoint joint;
        joint.name = sdk::json_string(jointValue, "name", "");
        joint.parent = sdk::json_string(jointValue, "parent", "");
        if (!read_vec3(jointValue, "anchor", joint.anchor)) {
            errorOut = "ragdoll asset: joint anchor must be a finite [x,y,z] array";
            return false;
        }
        if (!read_quat(jointValue, "rotation", joint.rotation)) {
            errorOut = "ragdoll asset: joint rotation must be a finite [x,y,z,w] array";
            return false;
        }
        joint.length = static_cast<float>(sdk::json_number(jointValue, "length", 0.5));
        joint.radius = static_cast<float>(sdk::json_number(jointValue, "radius", 0.12));
        joint.mass = static_cast<float>(sdk::json_number(jointValue, "mass", 1.0));

        const sdk::JsonValue* limitsValue = jointValue.field("limits");
        if (limitsValue != nullptr) {
            if (!limitsValue->is_object()) {
                errorOut = "ragdoll asset: joint limits must be an object";
                return false;
            }
            joint.limits.swingLimitX =
                static_cast<float>(sdk::json_number(*limitsValue, "swingLimitX", 0.0));
            joint.limits.swingLimitY =
                static_cast<float>(sdk::json_number(*limitsValue, "swingLimitY", 0.0));
            joint.limits.twistLimit =
                static_cast<float>(sdk::json_number(*limitsValue, "twistLimit", 0.0));
        }
        const sdk::JsonValue* driveValue = jointValue.field("drive");
        if (driveValue != nullptr) {
            if (!driveValue->is_object()) {
                errorOut = "ragdoll asset: joint drive must be an object";
                return false;
            }
            joint.drive.enabled = sdk::json_bool(*driveValue, "enabled", false);
            joint.drive.frequency =
                static_cast<float>(sdk::json_number(*driveValue, "frequency", 8.0));
            joint.drive.damping =
                static_cast<float>(sdk::json_number(*driveValue, "damping", 1.0));
        }
        parsed.joints.push_back(std::move(joint));
    }

    if (!check_asset(parsed, errorOut)) return false;
    *this = std::move(parsed);
    return true;
}

std::string RagdollAsset::to_json() const {
    std::ostringstream out;
    out << "{\"version\":" << version << ",\"name\":\"" << json_escape(name)
        << "\",\"autoBalance\":" << (autoBalance ? "true" : "false")
        << ",\"blend\":{\"recoverRate\":" << emit_float(blend.recoverRate)
        << "},\"joints\":[";
    for (std::size_t i = 0; i < joints.size(); ++i) {
        if (i != 0) out << ",";
        const RagdollJoint& j = joints[i];
        out << "{\"name\":\"" << json_escape(j.name) << "\",\"parent\":\""
            << json_escape(j.parent) << "\",\"anchor\":" << emit_vec3(j.anchor)
            << ",\"rotation\":" << emit_quat(j.rotation)
            << ",\"length\":" << emit_float(j.length)
            << ",\"radius\":" << emit_float(j.radius)
            << ",\"mass\":" << emit_float(j.mass)
            << ",\"limits\":{\"swingLimitX\":" << emit_float(j.limits.swingLimitX)
            << ",\"swingLimitY\":" << emit_float(j.limits.swingLimitY)
            << ",\"twistLimit\":" << emit_float(j.limits.twistLimit)
            << "},\"drive\":{\"enabled\":" << (j.drive.enabled ? "true" : "false")
            << ",\"frequency\":" << emit_float(j.drive.frequency)
            << ",\"damping\":" << emit_float(j.drive.damping) << "}}";
    }
    out << "]}";
    return out.str();
}

bool RagdollAsset::validate(std::string& errorOut) const {
    return check_asset(*this, errorOut);
}

std::vector<RagdollBone> RagdollAsset::build_bones() const {
    std::vector<RagdollBone> bones;
    bones.reserve(joints.size());
    for (const RagdollJoint& joint : joints) {
        RagdollBone bone;
        bone.name = joint.name;
        bone.parent = joint.parent;
        bone.position = joint.anchor;
        bone.rotation = joint.rotation;
        bone.length = joint.length;
        bone.radius = joint.radius;
        bone.mass = joint.mass;
        bones.push_back(std::move(bone));
    }
    return bones;
}

}  // namespace gameplay
}  // namespace engine
