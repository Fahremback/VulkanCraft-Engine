// Retargeting.cpp — adapter único de IRetargeting (engine::animation).
// Mapeia poses da skeleton fonte para a alvo: ossos mapeados recebem o local
// da fonte com posição × mapping.scale; ossos sem mapeamento ficam no bind
// da alvo (IAnimCore::bind_pose). JSON bit-exact all-or-nothing.

#include "engine/animation/IRetargeting.hpp"

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

struct RetargetData {
    std::string source_skeleton;
    std::string target_skeleton;
    std::vector<RetargetMapping> mappings;  // ordem de declaração
};

class Retargeting final : public IRetargeting {
public:
    explicit Retargeting(IAnimCore& core) : core_(core) {}

    // Valida SEM mutar o registro (compartilhada por add_retarget e pelo
    // restore all-or-nothing).
    bool validate_retarget(const std::string& retargetId,
                           const std::string& sourceSkeleton,
                           const std::string& targetSkeleton,
                           const std::vector<RetargetMapping>& mappings,
                           std::string& errorOut) const {
        if (retargetId.empty()) {
            errorOut = "retarget id must not be empty";
            return false;
        }
        if (retargets_.count(retargetId) != 0) {
            errorOut = "duplicate retarget id \"" + retargetId + "\"";
            return false;
        }
        if (!core_.has_skeleton(sourceSkeleton)) {
            errorOut = "unknown source skeleton \"" + sourceSkeleton + "\"";
            return false;
        }
        if (!core_.has_skeleton(targetSkeleton)) {
            errorOut = "unknown target skeleton \"" + targetSkeleton + "\"";
            return false;
        }
        std::string err;
        const std::vector<BonePose> srcBones =
            core_.bind_pose(sourceSkeleton, err);
        std::map<std::string, bool> srcSet;
        for (const BonePose& b : srcBones) srcSet[b.bone] = true;
        const std::vector<BonePose> dstBones =
            core_.bind_pose(targetSkeleton, err);
        std::map<std::string, bool> dstSet;
        for (const BonePose& b : dstBones) dstSet[b.bone] = true;

        std::map<std::string, bool> seenTarget;
        for (const RetargetMapping& m : mappings) {
            if (srcSet.count(m.source_bone) == 0) {
                errorOut = "unknown source bone \"" + m.source_bone +
                           "\" in skeleton \"" + sourceSkeleton + "\"";
                return false;
            }
            if (dstSet.count(m.target_bone) == 0) {
                errorOut = "unknown target bone \"" + m.target_bone +
                           "\" in skeleton \"" + targetSkeleton + "\"";
                return false;
            }
            if (seenTarget.count(m.target_bone) != 0) {
                errorOut = "duplicate target bone \"" + m.target_bone +
                           "\" in retarget \"" + retargetId + "\"";
                return false;
            }
            if (!std::isfinite(m.scale) || m.scale <= 0.0) {
                errorOut = "mapping scale must be finite and > 0";
                return false;
            }
            seenTarget[m.target_bone] = true;
        }
        errorOut.clear();
        return true;
    }

    bool add_retarget(const std::string& retargetId,
                      const std::string& sourceSkeleton,
                      const std::string& targetSkeleton,
                      const std::vector<RetargetMapping>& mappings,
                      std::string& errorOut) override {
        if (!validate_retarget(retargetId, sourceSkeleton, targetSkeleton,
                               mappings, errorOut)) {
            return false;
        }
        RetargetData data;
        data.source_skeleton = sourceSkeleton;
        data.target_skeleton = targetSkeleton;
        data.mappings = mappings;
        retargets_[retargetId] = std::move(data);
        errorOut.clear();
        return true;
    }

    bool has_retarget(const std::string& retargetId) const override {
        return retargets_.count(retargetId) != 0;
    }

    std::vector<std::string> retarget_ids() const override {
        std::vector<std::string> out;
        for (const auto& kv : retargets_) out.push_back(kv.first);
        return out;
    }

    std::vector<BonePose> retarget_pose(const std::string& retargetId,
                                        const std::string& clipId, double t,
                                        std::string& errorOut) const override {
        const auto it = retargets_.find(retargetId);
        if (it == retargets_.end()) {
            errorOut = "unknown retarget \"" + retargetId + "\"";
            return {};
        }
        const RetargetData& data = it->second;
        // O clip precisa estar registrado NA skeleton fonte.
        std::string clipErr;
        const double dur = core_.clip_duration(clipId, clipErr);
        if (!clipErr.empty()) {
            errorOut = clipErr;
            return {};
        }
        (void)dur;
        const std::vector<BonePose> sampled =
            core_.sample_clip(clipId, t, errorOut);
        if (!errorOut.empty()) return {};
        // Skeleton do clip: verifica se cada osso da fonte amostrada existe
        // na bind da fonte (clip de outra skeleton → erro honesto).
        std::string bindErr;
        const std::vector<BonePose> srcBind =
            core_.bind_pose(data.source_skeleton, bindErr);
        if (!bindErr.empty()) {
            errorOut = bindErr;
            return {};
        }
        std::map<std::string, bool> srcBindSet;
        for (const BonePose& b : srcBind) srcBindSet[b.bone] = true;
        for (const BonePose& p : sampled) {
            if (srcBindSet.count(p.bone) == 0) {
                errorOut = "clip \"" + clipId +
                           "\" is not registered on source skeleton \"" +
                           data.source_skeleton + "\"";
                return {};
            }
        }

        // Base = bind da alvo; aplica os mapeamentos.
        std::vector<BonePose> out =
            core_.bind_pose(data.target_skeleton, errorOut);
        if (!errorOut.empty()) return {};
        std::map<std::string, const BonePose*> sampledMap;
        for (const BonePose& p : sampled) sampledMap[p.bone] = &p;
        for (const RetargetMapping& m : data.mappings) {
            const auto sit = sampledMap.find(m.source_bone);
            if (sit == sampledMap.end()) continue;  // não deveria ocorrer
            for (BonePose& pose : out) {
                if (pose.bone != m.target_bone) continue;
                pose.local.position = {
                    sit->second->local.position.x * m.scale,
                    sit->second->local.position.y * m.scale,
                    sit->second->local.position.z * m.scale,
                };
                pose.local.rotation = sit->second->local.rotation;
                pose.local.scale = sit->second->local.scale;
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
        for (const auto& kv : retargets_) {
            if (!first) out << ",";
            first = false;
            const RetargetData& d = kv.second;
            out << "\"" << json_escape(kv.first) << "\":{\"source\":\""
                << json_escape(d.source_skeleton) << "\",\"target\":\""
                << json_escape(d.target_skeleton) << "\",\"mappings\":[";
            for (std::size_t i = 0; i < d.mappings.size(); ++i) {
                if (i > 0) out << ",";
                out << "{\"source\":\"" << json_escape(d.mappings[i].source_bone)
                    << "\",\"target\":\"" << json_escape(d.mappings[i].target_bone)
                    << "\",\"scale\":" << d.mappings[i].scale << "}";
            }
            out << "]}";
        }
        out << "}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) return false;
        if (!doc.is_object()) {
            errorOut = "retargeting state must be an object";
            return false;
        }
        std::map<std::string, RetargetData> parsed;
        for (const auto& kv : doc.object) {
            const std::string& id = kv.first;
            const sdk::JsonValue& entry = kv.second;
            if (!entry.is_object()) {
                errorOut = "retarget \"" + id + "\" must be an object";
                return false;
            }
            const sdk::JsonValue* src = entry.field("source");
            const sdk::JsonValue* dst = entry.field("target");
            const sdk::JsonValue* maps = entry.field("mappings");
            if (src == nullptr || dst == nullptr || maps == nullptr ||
                src->kind != sdk::JsonValue::Kind::String ||
                dst->kind != sdk::JsonValue::Kind::String ||
                maps->kind != sdk::JsonValue::Kind::Array) {
                errorOut = "retarget entry needs source/target strings and "
                           "mappings array";
                return false;
            }
            RetargetData data;
            data.source_skeleton = src->string;
            data.target_skeleton = dst->string;
            for (const sdk::JsonValue& m : maps->array) {
                if (!m.is_object()) {
                    errorOut = "mapping entry must be an object";
                    return false;
                }
                const sdk::JsonValue* mb = m.field("source");
                const sdk::JsonValue* tb = m.field("target");
                const sdk::JsonValue* sc = m.field("scale");
                if (mb == nullptr || tb == nullptr || sc == nullptr ||
                    mb->kind != sdk::JsonValue::Kind::String ||
                    tb->kind != sdk::JsonValue::Kind::String ||
                    sc->kind != sdk::JsonValue::Kind::Number) {
                    errorOut = "mapping entry needs source/target strings "
                               "and scale number";
                    return false;
                }
                RetargetMapping rm;
                rm.source_bone = mb->string;
                rm.target_bone = tb->string;
                rm.scale = sc->number;
                data.mappings.push_back(rm);
            }
            parsed[id] = std::move(data);
        }
        // Validação completa all-or-nothing SEM mutar (inclui id duplicado
        // entre as entradas do próprio estado).
        std::map<std::string, bool> seenIds;
        for (const auto& kv : parsed) {
            if (seenIds.count(kv.first) != 0) {
                errorOut = "duplicate retarget id in state";
                return false;
            }
            seenIds[kv.first] = true;
            const RetargetData& d = kv.second;
            std::string err;
            if (!validate_retarget(kv.first, d.source_skeleton,
                                   d.target_skeleton, d.mappings, err)) {
                errorOut = err;
                return false;
            }
        }
        // Commit único após validar TUDO.
        retargets_ = std::move(parsed);
        errorOut.clear();
        return true;
    }

private:
    IAnimCore& core_;
    std::map<std::string, RetargetData> retargets_;
};

}  // namespace

std::unique_ptr<IRetargeting> create_retargeting(IAnimCore& core) {
    return std::unique_ptr<IRetargeting>(new Retargeting(core));
}

}  // namespace engine::animation
