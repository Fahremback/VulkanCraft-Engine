// AnimAdditive.cpp — adapter único de IAnimAdditive (engine::animation).
// Sem estado mutável: deltas derivados de IAnimCore::sample_clip; camada
// aplica por correspondência de osso com all-or-nothing em osso desconhecido.

#include "engine/animation/IAnimAdditive.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <cmath>
#include <map>

namespace engine::animation {
namespace {

class AnimAdditive final : public IAnimAdditive {
public:
    explicit AnimAdditive(IAnimCore& core) : core_(core) {}

    std::vector<AdditiveDelta> sample_additive(
        const std::string& clipId, double t, double refTime,
        std::string& errorOut) const override {
        if (!std::isfinite(t) || !std::isfinite(refTime)) {
            errorOut = "sample/ref times must be finite";
            return {};
        }
        const std::vector<BonePose> pt = core_.sample_clip(clipId, t, errorOut);
        if (!errorOut.empty()) return {};
        const std::vector<BonePose> pref =
            core_.sample_clip(clipId, refTime, errorOut);
        if (!errorOut.empty()) return {};

        std::vector<AdditiveDelta> out;
        out.reserve(pt.size());
        for (std::size_t i = 0; i < pt.size(); ++i) {
            const AnimTransform& a = pt[i].local;
            const AnimTransform& b = pref[i].local;
            AdditiveDelta d;
            d.bone = pt[i].bone;
            d.local.position = a.position - b.position;
            d.local.rotation = (a.rotation * b.rotation.inverse()).normalized();
            d.local.scale = {a.scale.x / b.scale.x, a.scale.y / b.scale.y,
                             a.scale.z / b.scale.z};
            out.push_back(d);
        }
        errorOut.clear();
        return out;
    }

    std::vector<BonePose> layer_additive(
        const std::vector<BonePose>& base,
        const std::vector<AdditiveDelta>& deltas,
        std::string& errorOut) const override {
        std::map<std::string, const BonePose*> byBone;
        for (const BonePose& p : base) byBone[p.bone] = &p;
        for (const AdditiveDelta& d : deltas) {
            if (byBone.count(d.bone) == 0) {
                errorOut = "delta references unknown bone \"" + d.bone + "\"";
                return {};
            }
        }
        std::vector<BonePose> out = base;
        for (BonePose& p : out) {
            for (const AdditiveDelta& d : deltas) {
                if (d.bone != p.bone) continue;
                p.local.position = p.local.position + d.local.position;
                p.local.rotation = (p.local.rotation * d.local.rotation)
                                       .normalized();
                p.local.scale = {p.local.scale.x * d.local.scale.x,
                                 p.local.scale.y * d.local.scale.y,
                                 p.local.scale.z * d.local.scale.z};
            }
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
            errorOut = "additive state must be an object";
            return false;
        }
        errorOut.clear();
        return true;
    }

private:
    IAnimCore& core_;
};

}  // namespace

std::unique_ptr<IAnimAdditive> create_anim_additive(IAnimCore& core) {
    return std::unique_ptr<IAnimAdditive>(new AnimAdditive(core));
}

}  // namespace engine::animation
