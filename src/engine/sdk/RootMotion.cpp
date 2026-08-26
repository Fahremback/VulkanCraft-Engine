#include "engine/animation/IRootMotion.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine {
namespace animation {
namespace {

const BonePose* find_pose(const std::vector<BonePose>& poses,
                          const std::string& bone) {
    for (const auto& p : poses) {
        if (p.bone == bone) {
            return &p;
        }
    }
    return nullptr;
}

}  // namespace

namespace {

class RootMotion final : public IRootMotion {
public:
    RootMotionSample compute(IAnimCore& core, const std::string& clipId,
                             const std::string& rootBone, double t0, double t1,
                             std::string& errorOut) override {
        RootMotionSample out;
        if (rootBone.empty()) {
            errorOut = "root bone must be non-empty";
            return out;
        }
        if (!std::isfinite(t0) || !std::isfinite(t1)) {
            errorOut = "sample times must be finite";
            return out;
        }
        const auto p0 = core.sample_clip(clipId, t0, errorOut);
        if (!errorOut.empty()) {
            return out;
        }
        const auto p1 = core.sample_clip(clipId, t1, errorOut);
        if (!errorOut.empty()) {
            return out;
        }
        const BonePose* b0 = find_pose(p0, rootBone);
        const BonePose* b1 = find_pose(p1, rootBone);
        if (b0 == nullptr || b1 == nullptr) {
            errorOut = "unknown root bone \"" + rootBone + "\" in clip \"" +
                       clipId + "\"";
            return out;
        }
        out.position_delta = b1->local.position - b0->local.position;
        out.rotation_delta = (b1->local.rotation * b0->local.rotation.inverse())
                                 .normalized();
        const auto& d = out.position_delta;
        out.distance =
            std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        out.horizontal_distance =
            std::sqrt(out.position_delta.x * out.position_delta.x +
                      out.position_delta.z * out.position_delta.z);
        errorOut.clear();
        return out;
    }

    std::string serialize_state() const override {
        return "{}";
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) {
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "root motion state must be an object";
            return false;
        }
        errorOut.clear();
        return true;
    }
};

}  // namespace

std::unique_ptr<IRootMotion> create_root_motion() {
    return std::make_unique<RootMotion>();
}

}  // namespace animation
}  // namespace engine
