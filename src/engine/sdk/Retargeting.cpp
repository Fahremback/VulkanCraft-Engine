// Retargeting.cpp — adapter do contrato engine::animation::IRetargeting
// (agente 4 §4 item 2, unidade "retargeting"). Retargeting determinístico de
// animação entre skeletons (fonte → alvo) sobre um IAnimCore: skeletons e
// clips validados no registro; pose reamostrada NA ORDEM da skeleton alvo;
// osso mapeado usa o local da fonte com posição × mapping.scale (rotação e
// escala do transform copiadas); osso sem mapeamento = bind local da alvo
// (via IAnimCore). Erros honestos p/ retarget/clip desconhecidos e clip de
// outra skeleton. JSON versionado all-or-nothing bit-exact.

#include "engine/animation/IRetargeting.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

namespace engine {
namespace animation {

namespace {

bool is_finite(double v) { return std::isfinite(v); }

std::string fmt_double(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
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

bool contains(const std::vector<std::string>& values, const std::string& v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}

// Bone ids of a skeleton, in declaration order (via bind_pose).
std::vector<std::string> bone_ids(IAnimCore& core, const std::string& skeleton,
                                  std::string& errorOut) {
    const auto pose = core.bind_pose(skeleton, errorOut);
    std::vector<std::string> ids;
    ids.reserve(pose.size());
    for (const auto& p : pose) ids.push_back(p.bone);
    return ids;
}

class Retargeting final : public IRetargeting {
public:
    explicit Retargeting(IAnimCore& core) : core_(core) {}

    bool add_retarget(const std::string& retargetId,
                      const std::string& sourceSkeleton,
                      const std::string& targetSkeleton,
                      const std::vector<RetargetMapping>& mappings,
                      std::string& errorOut) override {
        errorOut.clear();
        if (retargetId.empty()) {
            errorOut = "retarget id must not be empty";
            return false;
        }
        if (retargets_.count(retargetId) != 0) {
            errorOut = "duplicate retarget id \"" + retargetId + "\"";
            return false;
        }
        if (!core_.has_skeleton(sourceSkeleton)) {
            errorOut = "retarget \"" + retargetId +
                       "\" references unknown skeleton \"" + sourceSkeleton + "\"";
            return false;
        }
        if (!core_.has_skeleton(targetSkeleton)) {
            errorOut = "retarget \"" + retargetId +
                       "\" references unknown skeleton \"" + targetSkeleton + "\"";
            return false;
        }
        const auto srcBones = bone_ids(core_, sourceSkeleton, errorOut);
        if (!errorOut.empty()) return false;
        const auto dstBones = bone_ids(core_, targetSkeleton, errorOut);
        if (!errorOut.empty()) return false;

        // Valida tudo antes de mutar (all-or-nothing).
        std::vector<RetargetMapping> validated;
        std::vector<std::string> usedTargets;
        for (const auto& m : mappings) {
            if (m.source_bone.empty() || m.target_bone.empty()) {
                errorOut = "mapping entry needs source/target strings and scale number";
                return false;
            }
            if (!is_finite(m.scale) || m.scale <= 0.0) {
                errorOut = "mapping scale must be finite and > 0";
                return false;
            }
            if (!contains(srcBones, m.source_bone)) {
                errorOut = "bone \"" + m.source_bone +
                           "\" is not registered on source skeleton \"" +
                           sourceSkeleton + "\"";
                return false;
            }
            if (!contains(dstBones, m.target_bone)) {
                errorOut = "bone \"" + m.target_bone +
                           "\" is not registered on target skeleton \"" +
                           targetSkeleton + "\"";
                return false;
            }
            if (contains(usedTargets, m.target_bone)) {
                errorOut = "duplicate target bone \"" + m.target_bone + "\"";
                return false;
            }
            usedTargets.push_back(m.target_bone);
            validated.push_back(m);
        }
        retargets_[retargetId] =
            Retarget{ sourceSkeleton, targetSkeleton, std::move(validated) };
        return true;
    }

    bool has_retarget(const std::string& retargetId) const override {
        return retargets_.count(retargetId) != 0;
    }

    std::vector<std::string> retarget_ids() const override {
        std::vector<std::string> ids;
        ids.reserve(retargets_.size());
        for (const auto& [id, rt] : retargets_) ids.push_back(id);
        return ids;
    }

    std::vector<BonePose> retarget_pose(const std::string& retargetId,
                                        const std::string& clipId, double t,
                                        std::string& errorOut) const override {
        errorOut.clear();
        const auto it = retargets_.find(retargetId);
        if (it == retargets_.end()) {
            errorOut = "unknown retarget \"" + retargetId + "\"";
            return {};
        }
        const auto& rt = it->second;
        if (!core_.has_clip(clipId)) {
            errorOut = "unknown clip \"" + clipId + "\"";
            return {};
        }
        // Amostra o clip (registrado na skeleton fonte). O IAnimCore não
        // expõe a skeleton de um clip; a pose amostrada cobre os ossos da
        // skeleton do clip — comparar com a skeleton fonte detecta clip de
        // outra skeleton (ordem e ids).
        const auto sourcePose = core_.sample_clip(clipId, t, errorOut);
        if (!errorOut.empty()) return {};
        const auto srcBones = bone_ids(core_, rt.source_skeleton, errorOut);
        if (!errorOut.empty()) return {};
        if (sourcePose.size() != srcBones.size()) {
            errorOut = "clip \"" + clipId + "\" is not registered on source skeleton \"" +
                       rt.source_skeleton + "\"";
            return {};
        }
        for (size_t i = 0; i < sourcePose.size(); ++i) {
            if (sourcePose[i].bone != srcBones[i]) {
                errorOut = "clip \"" + clipId +
                           "\" is not registered on source skeleton \"" +
                           rt.source_skeleton + "\"";
                return {};
            }
        }
        const auto dstBind = core_.bind_pose(rt.target_skeleton, errorOut);
        if (!errorOut.empty()) return {};

        // Pose na ordem da skeleton alvo; osso mapeado → local da fonte com
        // posição × scale; osso sem mapeamento → bind local da alvo.
        std::vector<BonePose> result;
        result.reserve(dstBind.size());
        for (const auto& bindPose : dstBind) {
            const RetargetMapping* map = nullptr;
            for (const auto& m : rt.mappings) {
                if (m.target_bone == bindPose.bone) {
                    map = &m;
                    break;
                }
            }
            if (map == nullptr) {
                result.push_back(bindPose);
                continue;
            }
            const BonePose* sourceLocal = nullptr;
            for (const auto& p : sourcePose) {
                if (p.bone == map->source_bone) {
                    sourceLocal = &p;
                    break;
                }
            }
            if (sourceLocal == nullptr) {
                // Invariante: o mapping foi validado contra a skeleton fonte.
                result.push_back(bindPose);
                continue;
            }
            BonePose out;
            out.bone = bindPose.bone;
            out.local = sourceLocal->local;
            out.local.position.x = sourceLocal->local.position.x * map->scale;
            out.local.position.y = sourceLocal->local.position.y * map->scale;
            out.local.position.z = sourceLocal->local.position.z * map->scale;
            result.push_back(std::move(out));
        }
        return result;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << "{\"version\":1,\"retargets\":{";
        bool first = true;
        for (const auto& [id, rt] : retargets_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(id) << "\":{\"source\":\""
                << json_escape(rt.source_skeleton) << "\",\"target\":\""
                << json_escape(rt.target_skeleton) << "\",\"mappings\":[";
            for (size_t i = 0; i < rt.mappings.size(); ++i) {
                if (i) out << ",";
                const auto& m = rt.mappings[i];
                out << "{\"source\":\"" << json_escape(m.source_bone)
                    << "\",\"target\":\"" << json_escape(m.target_bone)
                    << "\",\"scale\":" << fmt_double(m.scale) << "}";
            }
            out << "]}";
        }
        out << "}}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        errorOut.clear();
        engine::sdk::JsonValue root;
        if (!engine::sdk::json_parse(json, root, errorOut)) return false;
        if (!root.is_object()) {
            errorOut = "retargeting state must be an object";
            return false;
        }
        const auto* retargetsField = root.field("retargets");
        if (retargetsField == nullptr || !retargetsField->is_object()) {
            errorOut = "retargeting state must contain a retargets object";
            return false;
        }

        // Valida tudo numa estrutura temporária (all-or-nothing).
        std::map<std::string, Retarget> restored;
        for (const auto& [id, entry] : retargetsField->object) {
            if (restored.count(id) != 0) {
                errorOut = "duplicate retarget id in state";
                return false;
            }
            if (!entry.is_object()) {
                errorOut = "retarget entry must be an object";
                return false;
            }
            const std::string src = engine::sdk::json_string(entry, "source", "");
            const std::string dst = engine::sdk::json_string(entry, "target", "");
            if (src.empty() || dst.empty()) {
                errorOut = "retarget entry needs source/target strings and mappings array";
                return false;
            }
            if (!core_.has_skeleton(src)) {
                errorOut = "retarget \"" + id + "\" references unknown skeleton \"" +
                           src + "\"";
                return false;
            }
            if (!core_.has_skeleton(dst)) {
                errorOut = "retarget \"" + id + "\" references unknown skeleton \"" +
                           dst + "\"";
                return false;
            }
            const auto* mappingsField = entry.field("mappings");
            if (mappingsField == nullptr || !mappingsField->is_array()) {
                errorOut = "retarget entry needs source/target strings and mappings array";
                return false;
            }
            const auto srcBones = bone_ids(core_, src, errorOut);
            if (!errorOut.empty()) return false;
            const auto dstBones = bone_ids(core_, dst, errorOut);
            if (!errorOut.empty()) return false;
            std::vector<RetargetMapping> mappings;
            std::vector<std::string> usedTargets;
            for (const auto& m : mappingsField->array) {
                if (!m.is_object()) {
                    errorOut = "mapping entry must be an object";
                    return false;
                }
                const std::string sb = engine::sdk::json_string(m, "source", "");
                const std::string tb = engine::sdk::json_string(m, "target", "");
                const double scale = engine::sdk::json_number(m, "scale", 0.0);
                if (sb.empty() || tb.empty()) {
                    errorOut = "mapping entry needs source/target strings and scale number";
                    return false;
                }
                if (!is_finite(scale) || scale <= 0.0) {
                    errorOut = "mapping scale must be finite and > 0";
                    return false;
                }
                if (!contains(srcBones, sb)) {
                    errorOut = "bone \"" + sb +
                               "\" is not registered on source skeleton \"" + src + "\"";
                    return false;
                }
                if (!contains(dstBones, tb)) {
                    errorOut = "bone \"" + tb +
                               "\" is not registered on target skeleton \"" + dst + "\"";
                    return false;
                }
                if (contains(usedTargets, tb)) {
                    errorOut = "duplicate target bone \"" + tb + "\"";
                    return false;
                }
                usedTargets.push_back(tb);
                mappings.push_back(RetargetMapping{ sb, tb, scale });
            }
            restored[id] = Retarget{ src, dst, std::move(mappings) };
        }
        retargets_ = std::move(restored);
        return true;
    }

private:
    struct Retarget {
        std::string source_skeleton;
        std::string target_skeleton;
        std::vector<RetargetMapping> mappings;
    };

    IAnimCore& core_;
    std::map<std::string, Retarget> retargets_;
};

}  // namespace

std::unique_ptr<IRetargeting> create_retargeting(IAnimCore& core) {
    return std::make_unique<Retargeting>(core);
}

}  // namespace animation
}  // namespace engine
