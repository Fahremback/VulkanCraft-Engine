// Retargeting.cpp — the ONLY TU with the retargeting-editor behavior
// (agente 2 §B l.43). Pure document model: source/target skeleton ids +
// preserve-root-motion flag + bone mapping (source -> target with translation
// scale and rotation offset). All mutations are all-or-nothing (refused with
// a reason, document untouched). No clocks/RNG/globals. Deterministic JSON
// (%.6g floats).

#include "engine/editor/IRetargeting.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace engine {
namespace editor {

namespace {

bool is_finite(float v) {
    return std::isfinite(v);
}

std::string fmt(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

class RetargetingImpl final : public IRetargeting {
public:
    RetargetingImpl() = default;

    RetargetingSnapshot snapshot() const override {
        return { source_, target_, preserveRootMotion_, mapping_ };
    }

    bool set_skeletons(const std::string& source, const std::string& target,
                       std::string& errorOut) override {
        errorOut.clear();
        if (source.empty() || target.empty()) {
            errorOut = "source and target skeletons must not be empty";
            return false;
        }
        source_ = source;
        target_ = target;
        return true;
    }

    bool map(const RetargetBoneMapDef& m, std::string& errorOut) override {
        errorOut.clear();
        if (m.sourceBone.empty() || m.targetBone.empty()) {
            errorOut = "source and target bone names must not be empty";
            return false;
        }
        if (!is_finite(m.translationScale) || m.translationScale <= 0.0f) {
            errorOut = "translation scale must be positive and finite";
            return false;
        }
        if (!is_finite(m.rotationOffsetX) || !is_finite(m.rotationOffsetY) ||
            !is_finite(m.rotationOffsetZ)) {
            errorOut = "rotation offset must be finite";
            return false;
        }
        for (auto& existing : mapping_) {
            if (existing.sourceBone == m.sourceBone) {
                existing = m;
                return true;
            }
        }
        mapping_.push_back(m);
        return true;
    }

    bool unmap(const std::string& sourceBone, std::string& errorOut) override {
        errorOut.clear();
        const auto it = std::find_if(mapping_.begin(), mapping_.end(),
                                     [&](const RetargetBoneMapDef& m) {
                                         return m.sourceBone == sourceBone;
                                     });
        if (it == mapping_.end()) {
            errorOut = "no mapping for source bone: " + sourceBone;
            return false;
        }
        mapping_.erase(it);
        return true;
    }

    void clear_mapping() override {
        mapping_.clear();
    }

    void set_preserve_root_motion(bool preserve) override {
        preserveRootMotion_ = preserve;
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> issues;
        if (source_.empty() || target_.empty()) {
            issues.push_back("source and target skeletons are required");
        }
        if (mapping_.empty()) {
            issues.push_back("no bones mapped");
        }
        // Two source bones feeding the same target bone is ambiguous.
        for (size_t i = 0; i < mapping_.size(); ++i) {
            for (size_t j = i + 1; j < mapping_.size(); ++j) {
                if (mapping_[i].targetBone == mapping_[j].targetBone) {
                    issues.push_back("duplicate target bone: " +
                                     mapping_[i].targetBone);
                    break;
                }
            }
        }
        // Self mapping is legal but usually unintended.
        for (const auto& m : mapping_) {
            if (m.sourceBone == m.targetBone) {
                issues.push_back("self mapping: " + m.sourceBone);
            }
        }
        return issues;
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"sourceSkeleton\":\"" << json_escape(source_)
            << "\",\"targetSkeleton\":\"" << json_escape(target_)
            << "\",\"preserveRootMotion\":"
            << (preserveRootMotion_ ? "true" : "false")
            << ",\"mapping\":[";
        for (size_t i = 0; i < mapping_.size(); ++i) {
            if (i) out << ",";
            const auto& m = mapping_[i];
            out << "{\"sourceBone\":\"" << json_escape(m.sourceBone)
                << "\",\"targetBone\":\"" << json_escape(m.targetBone)
                << "\",\"translationScale\":" << fmt(m.translationScale)
                << ",\"rotationOffset\":[" << fmt(m.rotationOffsetX) << ","
                << fmt(m.rotationOffsetY) << "," << fmt(m.rotationOffsetZ)
                << "]}";
        }
        out << "]}";
        return out.str();
    }

private:
    std::string source_;
    std::string target_;
    bool preserveRootMotion_{ true };
    std::vector<RetargetBoneMapDef> mapping_;
};

}  // namespace

std::unique_ptr<IRetargeting> create_retargeting() {
    return std::make_unique<RetargetingImpl>();
}

}  // namespace editor
}  // namespace engine
