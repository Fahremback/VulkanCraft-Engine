// Constraints.cpp — adapter único de IConstraints (engine::animation).
// Extrai Euler XYZ (Tait-Bryan) de cada rotação limitada, clampa cada ângulo
// em [min,max] e reconstrói Qx·Qy·Qz. JSON bit-exact all-or-nothing.

#include "engine/animation/IConstraints.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace engine::animation {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::string json_escape(const std::string& text) {
    std::ostringstream out;
    for (char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c))
                        << std::dec;
                } else {
                    out << c;
                }
        }
    }
    return out.str();
}

double clamp_angle(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

struct EulerXyz {
    double x = 0.0, y = 0.0, z = 0.0;
};

// Extrai Euler XYZ (Tait-Bryan) de um quat (convenção R = Rx·Ry·Rz).
// Eixos x/z degenerados (s e c ~ 0 por cancelamento numérico) são fixados
// em 0 — evita atan2(0, ~0⁻) → ±π que quebraria a identidade de rotações
// puras em Y.
EulerXyz to_euler(const AnimQuat& q) {
    constexpr double kSnap = 1e-12;
    EulerXyz e;
    const double sx = 2.0 * (q.w * q.x + q.y * q.z);
    const double cx = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);
    e.x = (std::fabs(sx) < kSnap && std::fabs(cx) < kSnap)
              ? 0.0
              : std::atan2(sx, cx);
    const double sy = 2.0 * (q.w * q.y - q.z * q.x);
    e.y = std::asin(std::max(-1.0, std::min(1.0, sy)));
    const double sz = 2.0 * (q.w * q.z + q.x * q.y);
    const double cz = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
    e.z = (std::fabs(sz) < kSnap && std::fabs(cz) < kSnap)
              ? 0.0
              : std::atan2(sz, cz);
    return e;
}

// Reconstrói Qx(x)·Qy(y)·Qz(z).
AnimQuat from_euler(const EulerXyz& e) {
    const double hx = e.x * 0.5;
    const double hy = e.y * 0.5;
    const double hz = e.z * 0.5;
    const double cx = std::cos(hx), sx = std::sin(hx);
    const double cy = std::cos(hy), sy = std::sin(hy);
    const double cz = std::cos(hz), sz = std::sin(hz);
    // Qx·Qy·Qz (Hamilton): aplica Z, depois Y, depois X.
    const double qx = sx * cy * cz - cx * sy * sz;
    const double qy = cx * sy * cz + sx * cy * sz;
    const double qz = cx * cy * sz - sx * sy * cz;
    const double qw = cx * cy * cz + sx * sy * sz;
    return AnimQuat{qx, qy, qz, qw}.normalized();
}

class Constraints final : public IConstraints {
public:
    bool add_constraint(const std::string& constraintId,
                        const std::vector<JointLimit>& limits,
                        std::string& errorOut) override {
        if (constraintId.empty()) {
            errorOut = "constraint id must not be empty";
            return false;
        }
        if (constraints_.count(constraintId) != 0) {
            errorOut = "duplicate constraint id \"" + constraintId + "\"";
            return false;
        }
        std::vector<JointLimit> parsed;
        std::map<std::string, bool> seen;
        for (const JointLimit& l : limits) {
            if (l.bone.empty()) {
                errorOut = "limit bone must not be empty";
                return false;
            }
            if (seen.count(l.bone) != 0) {
                errorOut = "duplicate bone \"" + l.bone +
                           "\" in constraint \"" + constraintId + "\"";
                return false;
            }
            if (l.min_x > l.max_x || l.min_y > l.max_y || l.min_z > l.max_z) {
                errorOut = "limit min must be <= max on every axis";
                return false;
            }
            seen[l.bone] = true;
            parsed.push_back(l);
        }
        constraints_[constraintId] = std::move(parsed);
        errorOut.clear();
        return true;
    }

    bool has_constraint(const std::string& constraintId) const override {
        return constraints_.count(constraintId) != 0;
    }

    std::vector<BonePose> apply_constraint(
        const std::string& constraintId, const std::vector<BonePose>& pose,
        std::string& errorOut) const override {
        const auto it = constraints_.find(constraintId);
        if (it == constraints_.end()) {
            errorOut = "unknown constraint \"" + constraintId + "\"";
            return {};
        }
        std::map<std::string, bool> poseBones;
        for (const BonePose& p : pose) poseBones[p.bone] = true;
        for (const JointLimit& l : it->second) {
            if (poseBones.count(l.bone) == 0) {
                errorOut = "limit references unknown bone \"" + l.bone +
                           "\" in pose";
                return {};
            }
        }
        std::vector<BonePose> out = pose;
        for (BonePose& p : out) {
            for (const JointLimit& l : it->second) {
                if (l.bone != p.bone) continue;
                const EulerXyz e = to_euler(p.local.rotation);
                EulerXyz c;
                c.x = clamp_angle(e.x, l.min_x, l.max_x);
                c.y = clamp_angle(e.y, l.min_y, l.max_y);
                c.z = clamp_angle(e.z, l.min_z, l.max_z);
                p.local.rotation = from_euler(c);
            }
        }
        errorOut.clear();
        return out;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{";
        bool first = true;
        for (const auto& kv : constraints_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":[";
            for (std::size_t i = 0; i < kv.second.size(); ++i) {
                if (i > 0) out << ",";
                const JointLimit& l = kv.second[i];
                out << "{\"bone\":\"" << json_escape(l.bone)
                    << "\",\"min_x\":" << l.min_x << ",\"max_x\":" << l.max_x
                    << ",\"min_y\":" << l.min_y << ",\"max_y\":" << l.max_y
                    << ",\"min_z\":" << l.min_z << ",\"max_z\":" << l.max_z
                    << "}";
            }
            out << "]";
        }
        out << "}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) return false;
        if (!doc.is_object()) {
            errorOut = "constraints state must be an object";
            return false;
        }
        std::map<std::string, std::vector<JointLimit>> parsed;
        for (const auto& kv : doc.object) {
            const sdk::JsonValue& arr = kv.second;
            if (!arr.is_array()) {
                errorOut = "constraint \"" + kv.first +
                           "\" must be an array";
                return false;
            }
            std::vector<JointLimit> limits;
            for (const sdk::JsonValue& item : arr.array) {
                if (!item.is_object()) {
                    errorOut = "limit entry must be an object";
                    return false;
                }
                const sdk::JsonValue* bone = item.field("bone");
                const sdk::JsonValue* minx = item.field("min_x");
                const sdk::JsonValue* maxx = item.field("max_x");
                const sdk::JsonValue* miny = item.field("min_y");
                const sdk::JsonValue* maxy = item.field("max_y");
                const sdk::JsonValue* minz = item.field("min_z");
                const sdk::JsonValue* maxz = item.field("max_z");
                if (bone == nullptr || minx == nullptr || maxx == nullptr ||
                    miny == nullptr || maxy == nullptr || minz == nullptr ||
                    maxz == nullptr ||
                    bone->kind != sdk::JsonValue::Kind::String ||
                    minx->kind != sdk::JsonValue::Kind::Number ||
                    maxx->kind != sdk::JsonValue::Kind::Number ||
                    miny->kind != sdk::JsonValue::Kind::Number ||
                    maxy->kind != sdk::JsonValue::Kind::Number ||
                    minz->kind != sdk::JsonValue::Kind::Number ||
                    maxz->kind != sdk::JsonValue::Kind::Number) {
                    errorOut = "limit entry needs bone + 6 numeric bounds";
                    return false;
                }
                JointLimit l;
                l.bone = bone->string;
                l.min_x = minx->number;
                l.max_x = maxx->number;
                l.min_y = miny->number;
                l.max_y = maxy->number;
                l.min_z = minz->number;
                l.max_z = maxz->number;
                if (l.min_x > l.max_x || l.min_y > l.max_y ||
                    l.min_z > l.max_z) {
                    errorOut = "limit min must be <= max";
                    return false;
                }
                limits.push_back(l);
            }
            parsed[kv.first] = std::move(limits);
        }
        // Valida duplicatas de id e de osso por constraint.
        std::map<std::string, bool> seenIds;
        for (const auto& kv : parsed) {
            if (seenIds.count(kv.first) != 0) {
                errorOut = "duplicate constraint id in state";
                return false;
            }
            seenIds[kv.first] = true;
            std::map<std::string, bool> seenBones;
            for (const JointLimit& l : kv.second) {
                if (l.bone.empty()) {
                    errorOut = "limit bone must not be empty";
                    return false;
                }
                if (seenBones.count(l.bone) != 0) {
                    errorOut = "duplicate bone in constraint";
                    return false;
                }
                seenBones[l.bone] = true;
            }
        }
        constraints_ = std::move(parsed);
        errorOut.clear();
        return true;
    }

private:
    std::map<std::string, std::vector<JointLimit>> constraints_;
};

}  // namespace

std::unique_ptr<IConstraints> create_constraints() {
    return std::unique_ptr<IConstraints>(new Constraints());
}

}  // namespace engine::animation
