// AnimMask.cpp — adapter único de IAnimMask (engine::animation).
// Máscaras = map<maskId, map<bone, weight>>; mask_deltas aplica pesos aos
// deltas aditivos (#212): pos·w, rot = slerp(identidade, rot, w),
// scale = lerp(1, scale, w). JSON bit-exact all-or-nothing.

#include "engine/animation/IAnimMask.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace engine::animation {
namespace {

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

class AnimMask final : public IAnimMask {
public:
    bool add_mask(const std::string& maskId,
                  const std::vector<AnimMaskEntry>& entries,
                  std::string& errorOut) override {
        if (maskId.empty()) {
            errorOut = "mask id must not be empty";
            return false;
        }
        if (masks_.count(maskId) != 0) {
            errorOut = "duplicate mask id \"" + maskId + "\"";
            return false;
        }
        std::map<std::string, double> parsed;
        for (const AnimMaskEntry& e : entries) {
            if (e.bone.empty()) {
                errorOut = "mask bone must not be empty";
                return false;
            }
            if (!std::isfinite(e.weight) || e.weight < 0.0 ||
                e.weight > 1.0) {
                errorOut = "mask weight must be finite and in [0,1]";
                return false;
            }
            if (parsed.count(e.bone) != 0) {
                errorOut = "duplicate bone \"" + e.bone +
                           "\" in mask \"" + maskId + "\"";
                return false;
            }
            parsed[e.bone] = e.weight;
        }
        masks_[maskId] = std::move(parsed);
        errorOut.clear();
        return true;
    }

    bool has_mask(const std::string& maskId) const override {
        return masks_.count(maskId) != 0;
    }

    std::vector<std::string> mask_ids() const override {
        std::vector<std::string> out;
        for (const auto& kv : masks_) out.push_back(kv.first);
        return out;
    }

    double weight(const std::string& maskId,
                  const std::string& bone) const override {
        const auto it = masks_.find(maskId);
        if (it == masks_.end()) return 0.0;
        const auto bit = it->second.find(bone);
        if (bit == it->second.end()) return 0.0;
        return bit->second;
    }

    std::vector<AdditiveDelta> mask_deltas(
        const std::string& maskId, const std::vector<AdditiveDelta>& deltas,
        std::string& errorOut) const override {
        const auto it = masks_.find(maskId);
        if (it == masks_.end()) {
            errorOut = "unknown mask \"" + maskId + "\"";
            return {};
        }
        std::vector<AdditiveDelta> out;
        out.reserve(deltas.size());
        for (const AdditiveDelta& d : deltas) {
            const auto bit = it->second.find(d.bone);
            const double w = bit == it->second.end() ? 0.0 : bit->second;
            AdditiveDelta m;
            m.bone = d.bone;
            m.local.position = d.local.position * w;
            m.local.rotation =
                AnimQuat::slerp(AnimQuat{}, d.local.rotation, w);
            m.local.scale = {1.0 + (d.local.scale.x - 1.0) * w,
                             1.0 + (d.local.scale.y - 1.0) * w,
                             1.0 + (d.local.scale.z - 1.0) * w};
            out.push_back(m);
        }
        errorOut.clear();
        return out;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{";
        bool first = true;
        for (const auto& kv : masks_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":{";
            bool firstBone = true;
            for (const auto& bone : kv.second) {
                if (!firstBone) out << ",";
                firstBone = false;
                out << "\"" << json_escape(bone.first) << "\":" << bone.second;
            }
            out << "}";
        }
        out << "}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) return false;
        if (!doc.is_object()) {
            errorOut = "mask state must be an object";
            return false;
        }
        std::map<std::string, std::map<std::string, double>> parsed;
        for (const auto& kv : doc.object) {
            const std::string& maskId = kv.first;
            const sdk::JsonValue& bones = kv.second;
            if (!bones.is_object()) {
                errorOut = "mask \"" + maskId + "\" must be an object";
                return false;
            }
            std::map<std::string, double> parsedBones;
            for (const auto& bone : bones.object) {
                const std::string& boneName = bone.first;
                const sdk::JsonValue& w = bone.second;
                if (w.kind != sdk::JsonValue::Kind::Number) {
                    errorOut = "mask weight must be a number";
                    return false;
                }
                if (!std::isfinite(w.number) || w.number < 0.0 ||
                    w.number > 1.0) {
                    errorOut = "mask weight must be in [0,1]";
                    return false;
                }
                parsedBones[boneName] = w.number;
            }
            parsed[maskId] = std::move(parsedBones);
        }
        masks_ = std::move(parsed);
        errorOut.clear();
        return true;
    }

private:
    std::map<std::string, std::map<std::string, double>> masks_;
};

}  // namespace

std::unique_ptr<IAnimMask> create_anim_mask() {
    return std::unique_ptr<IAnimMask>(new AnimMask());
}

}  // namespace engine::animation
