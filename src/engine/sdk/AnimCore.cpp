#include "engine/animation/IAnimCore.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace engine {
namespace animation {
namespace {

bool finite(double v) {
    return std::isfinite(v);
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

bool is_uint64(const sdk::JsonValue& v) {
    return v.kind == sdk::JsonValue::Kind::Number && v.number >= 0.0 &&
           v.number == std::floor(v.number);
}

bool string_field(const sdk::JsonValue& obj, const char* key, std::string& out,
                  bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::String) {
        errorOut = std::string(key) + " must be a string";
        return false;
    }
    out = f->string;
    return true;
}

bool number_field(const sdk::JsonValue& obj, const char* key, double& out,
                  bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Number) {
        errorOut = std::string(key) + " must be a number";
        return false;
    }
    out = f->number;
    return true;
}

bool int_field(const sdk::JsonValue& obj, const char* key, int& out,
               bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Number) {
        errorOut = std::string(key) + " must be a number";
        return false;
    }
    const double v = f->number;
    if (v < -2147483648.0 || v > 2147483647.0 || v != std::floor(v)) {
        errorOut = std::string(key) + " must be a whole number";
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool vec3_field(const sdk::JsonValue& obj, const char* key, AnimVec3& out,
                bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (!f->is_array() || f->array.size() != 3) {
        errorOut = std::string(key) + " must be a [x,y,z] array";
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (f->array[i].kind != sdk::JsonValue::Kind::Number) {
            errorOut = std::string(key) + " entries must be numbers";
            return false;
        }
    }
    out = {f->array[0].number, f->array[1].number, f->array[2].number};
    return true;
}

bool quat_field(const sdk::JsonValue& obj, const char* key, AnimQuat& out,
                bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (!f->is_array() || f->array.size() != 4) {
        errorOut = std::string(key) + " must be a [x,y,z,w] array";
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (f->array[i].kind != sdk::JsonValue::Kind::Number) {
            errorOut = std::string(key) + " entries must be numbers";
            return false;
        }
    }
    out = {f->array[0].number, f->array[1].number, f->array[2].number,
           f->array[3].number};
    return true;
}

bool transform_field(const sdk::JsonValue& obj, AnimTransform& out,
                     std::string& errorOut) {
    if (!vec3_field(obj, "position", out.position, false, errorOut)) return false;
    if (!quat_field(obj, "rotation", out.rotation, false, errorOut)) return false;
    if (!vec3_field(obj, "scale", out.scale, false, errorOut)) return false;
    return true;
}

void emit_vec3(std::ostringstream& out, const AnimVec3& v) {
    out << "[" << v.x << "," << v.y << "," << v.z << "]";
}
void emit_quat(std::ostringstream& out, const AnimQuat& q) {
    out << "[" << q.x << "," << q.y << "," << q.z << "," << q.w << "]";
}
void emit_transform(std::ostringstream& out, const AnimTransform& t) {
    out << "{\"position\":";
    emit_vec3(out, t.position);
    out << ",\"rotation\":";
    emit_quat(out, t.rotation);
    out << ",\"scale\":";
    emit_vec3(out, t.scale);
    out << "}";
}

// --- Parsers internos (compartilhados por load_from_json e deserialize) ----

bool parse_version(const sdk::JsonValue& doc, std::string& errorOut) {
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported spec version";
        return false;
    }
    return true;
}

bool parse_skeleton(const sdk::JsonValue& doc, SkeletonSpec& out,
                    std::string& errorOut) {
    if (!doc.is_object()) {
        errorOut = "skeleton spec must be an object";
        return false;
    }
    if (!parse_version(doc, errorOut)) return false;
    SkeletonSpec candidate;
    if (!string_field(doc, "id", candidate.id, true, errorOut)) return false;
    const sdk::JsonValue* bonesField = doc.field("bones");
    if (bonesField == nullptr || !bonesField->is_array()) {
        errorOut = "skeleton must contain a bones array";
        return false;
    }
    for (const auto& item : bonesField->array) {
        if (!item.is_object()) {
            errorOut = "bone entries must be objects";
            return false;
        }
        Bone b;
        if (!string_field(item, "id", b.id, true, errorOut)) return false;
        if (!int_field(item, "parent", b.parent, false, errorOut)) return false;
        const sdk::JsonValue* bindField = item.field("bind_local");
        if (bindField != nullptr) {
            if (!bindField->is_object()) {
                errorOut = "bone bind_local must be an object";
                return false;
            }
            if (!transform_field(*bindField, b.bind_local, errorOut)) return false;
        }
        candidate.bones.push_back(b);
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    out = std::move(candidate);
    return true;
}

bool parse_clip(const sdk::JsonValue& doc, ClipSpec& out, std::string& errorOut) {
    if (!doc.is_object()) {
        errorOut = "clip spec must be an object";
        return false;
    }
    if (!parse_version(doc, errorOut)) return false;
    ClipSpec candidate;
    if (!string_field(doc, "id", candidate.id, true, errorOut)) return false;
    if (!string_field(doc, "skeleton", candidate.skeleton, true, errorOut))
        return false;
    if (!number_field(doc, "duration", candidate.duration, false, errorOut))
        return false;
    const sdk::JsonValue* tracksField = doc.field("tracks");
    if (tracksField != nullptr) {
        if (!tracksField->is_array()) {
            errorOut = "tracks must be an array";
            return false;
        }
        for (const auto& item : tracksField->array) {
            if (!item.is_object()) {
                errorOut = "track entries must be objects";
                return false;
            }
            BoneTrack tr;
            if (!string_field(item, "bone", tr.bone, true, errorOut)) return false;
            const sdk::JsonValue* keysField = item.field("keys");
            if (keysField == nullptr || !keysField->is_array()) {
                errorOut = "track must contain a keys array";
                return false;
            }
            for (const auto& kitem : keysField->array) {
                if (!kitem.is_object()) {
                    errorOut = "key entries must be objects";
                    return false;
                }
                Keyframe k;
                if (!number_field(kitem, "t", k.t, true, errorOut)) return false;
                const sdk::JsonValue* valueField = kitem.field("value");
                if (valueField == nullptr || !valueField->is_object()) {
                    errorOut = "key must contain a value object";
                    return false;
                }
                if (!transform_field(*valueField, k.value, errorOut)) return false;
                tr.keys.push_back(k);
            }
            candidate.tracks.push_back(tr);
        }
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    out = std::move(candidate);
    return true;
}

bool parse_blend(const sdk::JsonValue& doc, BlendSpec& out, std::string& errorOut) {
    if (!doc.is_object()) {
        errorOut = "blend spec must be an object";
        return false;
    }
    if (!parse_version(doc, errorOut)) return false;
    BlendSpec candidate;
    if (!string_field(doc, "id", candidate.id, true, errorOut)) return false;
    if (!string_field(doc, "clip_a", candidate.clip_a, true, errorOut)) return false;
    if (!string_field(doc, "clip_b", candidate.clip_b, true, errorOut)) return false;
    if (!number_field(doc, "param_min", candidate.param_min, false, errorOut))
        return false;
    if (!number_field(doc, "param_max", candidate.param_max, false, errorOut))
        return false;
    if (!candidate.validate(errorOut)) {
        return false;
    }
    out = std::move(candidate);
    return true;
}

}  // namespace

bool SkeletonSpec::validate(std::string& errorOut) const {
    if (id.empty()) {
        errorOut = "skeleton id must be non-empty";
        return false;
    }
    if (bones.empty()) {
        errorOut = "skeleton must have at least one bone";
        return false;
    }
    std::set<std::string> ids;
    int roots = 0;
    for (const auto& b : bones) {
        if (b.id.empty()) {
            errorOut = "bone id must be non-empty";
            return false;
        }
        if (ids.count(b.id)) {
            errorOut = "duplicate bone id \"" + b.id + "\"";
            return false;
        }
        ids.insert(b.id);
        if (b.parent < -1 || b.parent >= static_cast<int>(bones.size())) {
            errorOut = "bone \"" + b.id + "\" has out-of-range parent";
            return false;
        }
        if (b.parent == -1) {
            ++roots;
        }
    }
    if (roots == 0) {
        errorOut = "skeleton must have at least one root bone";
        return false;
    }
    for (const auto& b : bones) {
        std::set<int> seen;
        int cur = b.parent;
        while (cur != -1) {
            if (cur < 0 || cur >= static_cast<int>(bones.size()) ||
                !seen.insert(cur).second) {
                errorOut = "parent cycle detected at bone \"" + b.id + "\"";
                return false;
            }
            cur = bones[cur].parent;
        }
    }
    errorOut.clear();
    return true;
}

bool SkeletonSpec::load_from_json(const std::string& json, std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    return parse_skeleton(doc, *this, errorOut);
}

std::string SkeletonSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"id\":\"" << json_escape(id) << "\",\"bones\":[";
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (i) out << ",";
        out << "{\"id\":\"" << json_escape(bones[i].id) << "\",\"parent\":"
            << bones[i].parent << ",\"bind_local\":";
        emit_transform(out, bones[i].bind_local);
        out << "}";
    }
    out << "]}";
    return out.str();
}

bool ClipSpec::validate(std::string& errorOut) const {
    if (id.empty()) {
        errorOut = "clip id must be non-empty";
        return false;
    }
    if (skeleton.empty()) {
        errorOut = "clip skeleton must be non-empty";
        return false;
    }
    if (!finite(duration) || duration <= 0.0) {
        errorOut = "clip duration must be finite and > 0";
        return false;
    }
    std::set<std::string> bones;
    for (const auto& tr : tracks) {
        if (tr.bone.empty()) {
            errorOut = "track bone must be non-empty";
            return false;
        }
        if (bones.count(tr.bone)) {
            errorOut = "duplicate track for bone \"" + tr.bone + "\"";
            return false;
        }
        bones.insert(tr.bone);
        if (tr.keys.empty()) {
            errorOut = "track \"" + tr.bone + "\" must have at least one key";
            return false;
        }
        double prev = -1.0;
        for (const auto& k : tr.keys) {
            if (!finite(k.t) || k.t < 0.0 || k.t > duration) {
                errorOut =
                    "track \"" + tr.bone + "\" key time out of [0,duration]";
                return false;
            }
            if (k.t <= prev) {
                errorOut =
                    "track \"" + tr.bone +
                    "\" key times must be strictly increasing";
                return false;
            }
            prev = k.t;
        }
    }
    errorOut.clear();
    return true;
}

bool ClipSpec::load_from_json(const std::string& json, std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    return parse_clip(doc, *this, errorOut);
}

std::string ClipSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"id\":\"" << json_escape(id) << "\",\"skeleton\":\""
        << json_escape(skeleton) << "\",\"duration\":" << duration
        << ",\"tracks\":[";
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (i) out << ",";
        const auto& tr = tracks[i];
        out << "{\"bone\":\"" << json_escape(tr.bone) << "\",\"keys\":[";
        for (std::size_t j = 0; j < tr.keys.size(); ++j) {
            if (j) out << ",";
            out << "{\"t\":" << tr.keys[j].t << ",\"value\":";
            emit_transform(out, tr.keys[j].value);
            out << "}";
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

bool BlendSpec::validate(std::string& errorOut) const {
    if (id.empty()) {
        errorOut = "blend id must be non-empty";
        return false;
    }
    if (clip_a.empty() || clip_b.empty()) {
        errorOut = "blend clip ids must be non-empty";
        return false;
    }
    if (!finite(param_min) || !finite(param_max) || param_max <= param_min) {
        errorOut = "blend param range must be finite with param_max > param_min";
        return false;
    }
    errorOut.clear();
    return true;
}

bool BlendSpec::load_from_json(const std::string& json, std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    return parse_blend(doc, *this, errorOut);
}

std::string BlendSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"id\":\"" << json_escape(id) << "\",\"clip_a\":\""
        << json_escape(clip_a) << "\",\"clip_b\":\"" << json_escape(clip_b)
        << "\",\"param_min\":" << param_min << ",\"param_max\":" << param_max
        << "}";
    return out.str();
}

namespace {

AnimTransform sample_track(const BoneTrack& track, double t) {
    const std::vector<Keyframe>& keys = track.keys;
    if (t <= keys.front().t) return keys.front().value;
    if (t >= keys.back().t) return keys.back().value;
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        if (t < keys[i + 1].t) {
            const double span = keys[i + 1].t - keys[i].t;
            const double f = (span > 0.0) ? (t - keys[i].t) / span : 0.0;
            return AnimTransform::lerp(keys[i].value, keys[i + 1].value, f);
        }
    }
    return keys.back().value;
}

class AnimCore final : public IAnimCore {
public:
    bool add_skeleton(const SkeletonSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        if (skeletons_.count(spec.id)) {
            errorOut = "duplicate skeleton \"" + spec.id + "\"";
            return false;
        }
        skeletons_[spec.id] = spec;
        errorOut.clear();
        return true;
    }

    bool add_clip(const ClipSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        if (clips_.count(spec.id)) {
            errorOut = "duplicate clip \"" + spec.id + "\"";
            return false;
        }
        if (!skeletons_.count(spec.skeleton)) {
            errorOut = "clip \"" + spec.id + "\" references unknown skeleton \"" +
                       spec.skeleton + "\"";
            return false;
        }
        clips_[spec.id] = spec;
        errorOut.clear();
        return true;
    }

    bool add_blend(const BlendSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        if (blends_.count(spec.id)) {
            errorOut = "duplicate blend \"" + spec.id + "\"";
            return false;
        }
        const auto ita = clips_.find(spec.clip_a);
        const auto itb = clips_.find(spec.clip_b);
        if (ita == clips_.end()) {
            errorOut = "blend \"" + spec.id + "\" references unknown clip \"" +
                       spec.clip_a + "\"";
            return false;
        }
        if (itb == clips_.end()) {
            errorOut = "blend \"" + spec.id + "\" references unknown clip \"" +
                       spec.clip_b + "\"";
            return false;
        }
        if (ita->second.skeleton != itb->second.skeleton) {
            errorOut = "blend \"" + spec.id +
                       "\" clips must share the same skeleton";
            return false;
        }
        blends_[spec.id] = spec;
        errorOut.clear();
        return true;
    }

    bool has_skeleton(const std::string& id) const override {
        return skeletons_.count(id) != 0;
    }
    bool has_clip(const std::string& id) const override {
        return clips_.count(id) != 0;
    }
    bool has_blend(const std::string& id) const override {
        return blends_.count(id) != 0;
    }

    double clip_duration(const std::string& clipId,
                         std::string& errorOut) override {
        const auto cit = clips_.find(clipId);
        if (cit == clips_.end()) {
            errorOut = "unknown clip \"" + clipId + "\"";
            return 0.0;
        }
        errorOut.clear();
        return cit->second.duration;
    }

    std::vector<BonePose> bind_pose(const std::string& skeletonId,
                                    std::string& errorOut) override {
        const auto sit = skeletons_.find(skeletonId);
        if (sit == skeletons_.end()) {
            errorOut = "unknown skeleton \"" + skeletonId + "\"";
            return {};
        }
        std::vector<BonePose> out;
        for (const Bone& b : sit->second.bones) {
            out.push_back(BonePose{b.id, b.bind_local});
        }
        errorOut.clear();
        return out;
    }

    std::vector<std::string> skeleton_ids() const override {
        std::vector<std::string> out;
        for (const auto& kv : skeletons_) out.push_back(kv.first);
        return out;
    }
    std::vector<std::string> clip_ids() const override {
        std::vector<std::string> out;
        for (const auto& kv : clips_) out.push_back(kv.first);
        return out;
    }
    std::vector<std::string> blend_ids() const override {
        std::vector<std::string> out;
        for (const auto& kv : blends_) out.push_back(kv.first);
        return out;
    }

    std::vector<BonePose> sample_clip(const std::string& clipId, double t,
                                      std::string& errorOut) override {
        const auto cit = clips_.find(clipId);
        if (cit == clips_.end()) {
            errorOut = "unknown clip \"" + clipId + "\"";
            return {};
        }
        if (!finite(t)) {
            errorOut = "sample time must be finite";
            return {};
        }
        const ClipSpec& clip = cit->second;
        const SkeletonSpec& sk = skeletons_.at(clip.skeleton);
        const double tc = std::max(0.0, std::min(clip.duration, t));
        std::vector<BonePose> out;
        out.reserve(sk.bones.size());
        for (const auto& bone : sk.bones) {
            const BoneTrack* track = nullptr;
            for (const auto& tr : clip.tracks) {
                if (tr.bone == bone.id) {
                    track = &tr;
                    break;
                }
            }
            const AnimTransform local =
                (track != nullptr) ? sample_track(*track, tc) : bone.bind_local;
            out.push_back(BonePose{bone.id, local});
        }
        errorOut.clear();
        return out;
    }

    std::vector<BonePose> sample_blend(const std::string& blendId, double param,
                                       double t, std::string& errorOut) override {
        const auto bit = blends_.find(blendId);
        if (bit == blends_.end()) {
            errorOut = "unknown blend \"" + blendId + "\"";
            return {};
        }
        if (!finite(param) || !finite(t)) {
            errorOut = "blend param/time must be finite";
            return {};
        }
        const BlendSpec& blend = bit->second;
        double w = (param - blend.param_min) / (blend.param_max - blend.param_min);
        w = std::max(0.0, std::min(1.0, w));
        const auto& ca = clips_.at(blend.clip_a);
        const auto& cb = clips_.at(blend.clip_b);
        const auto pa = sample_clip(blend.clip_a, t * ca.duration, errorOut);
        const auto pb = sample_clip(blend.clip_b, t * cb.duration, errorOut);
        if (pa.size() != pb.size()) {
            errorOut = "blend clips have incompatible skeletons";
            return {};
        }
        std::vector<BonePose> out;
        out.reserve(pa.size());
        for (std::size_t i = 0; i < pa.size(); ++i) {
            out.push_back(
                BonePose{pa[i].bone, AnimTransform::lerp(pa[i].local, pb[i].local, w)});
        }
        errorOut.clear();
        return out;
    }

    std::vector<WorldPose> local_to_world(const std::string& skeletonId,
                                          const std::vector<BonePose>& poses,
                                          std::string& errorOut) override {
        const auto sit = skeletons_.find(skeletonId);
        if (sit == skeletons_.end()) {
            errorOut = "unknown skeleton \"" + skeletonId + "\"";
            return {};
        }
        const SkeletonSpec& sk = sit->second;
        if (poses.size() != sk.bones.size()) {
            errorOut = "poses must cover every bone of the skeleton";
            return {};
        }
        std::vector<WorldPose> out;
        out.reserve(poses.size());
        for (std::size_t i = 0; i < sk.bones.size(); ++i) {
            const Bone& bone = sk.bones[i];
            const AnimTransform& local = poses[i].local;
            if (bone.parent == -1) {
                out.push_back(WorldPose{bone.id, local});
            } else {
                const WorldPose& p = out[bone.parent];
                const AnimVec3 scaled_pos = local.position * p.world.scale;
                const AnimVec3 world_pos =
                    p.world.rotation.rotate(scaled_pos) + p.world.position;
                const AnimQuat world_rot = p.world.rotation * local.rotation;
                const AnimVec3 world_scale = p.world.scale * local.scale;
                out.push_back(
                    WorldPose{bone.id, {world_pos, world_rot, world_scale}});
            }
        }
        errorOut.clear();
        return out;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << "{\"skeletons\":[";
        bool first = true;
        for (const auto& kv : skeletons_) {
            if (!first) out << ",";
            first = false;
            out << kv.second.to_json();
        }
        out << "],\"clips\":[";
        first = true;
        for (const auto& kv : clips_) {
            if (!first) out << ",";
            first = false;
            out << kv.second.to_json();
        }
        out << "],\"blends\":[";
        first = true;
        for (const auto& kv : blends_) {
            if (!first) out << ",";
            first = false;
            out << kv.second.to_json();
        }
        out << "]}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) {
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "anim core state must be an object";
            return false;
        }
        // all-or-nothing: monta tudo em temporário e valida cross-refs.
        std::map<std::string, SkeletonSpec> nskeletons;
        std::map<std::string, ClipSpec> nclips;
        std::map<std::string, BlendSpec> nblends;

        const sdk::JsonValue* skField = doc.field("skeletons");
        if (skField == nullptr || !skField->is_array()) {
            errorOut = "state must contain a skeletons array";
            return false;
        }
        for (const auto& item : skField->array) {
            SkeletonSpec s;
            if (!parse_skeleton(item, s, errorOut)) {
                return false;
            }
            if (nskeletons.count(s.id)) {
                errorOut = "duplicate skeleton in state \"" + s.id + "\"";
                return false;
            }
            nskeletons[s.id] = s;
        }
        const sdk::JsonValue* clipField = doc.field("clips");
        if (clipField == nullptr || !clipField->is_array()) {
            errorOut = "state must contain a clips array";
            return false;
        }
        for (const auto& item : clipField->array) {
            ClipSpec c;
            if (!parse_clip(item, c, errorOut)) {
                return false;
            }
            if (nclips.count(c.id)) {
                errorOut = "duplicate clip in state \"" + c.id + "\"";
                return false;
            }
            if (!nskeletons.count(c.skeleton)) {
                errorOut = "clip \"" + c.id + "\" references unknown skeleton \"" +
                           c.skeleton + "\"";
                return false;
            }
            nclips[c.id] = c;
        }
        const sdk::JsonValue* blendField = doc.field("blends");
        if (blendField == nullptr || !blendField->is_array()) {
            errorOut = "state must contain a blends array";
            return false;
        }
        for (const auto& item : blendField->array) {
            BlendSpec b;
            if (!parse_blend(item, b, errorOut)) {
                return false;
            }
            if (nblends.count(b.id)) {
                errorOut = "duplicate blend in state \"" + b.id + "\"";
                return false;
            }
            if (!nclips.count(b.clip_a) || !nclips.count(b.clip_b)) {
                errorOut = "blend \"" + b.id + "\" references unknown clip";
                return false;
            }
            if (nclips[b.clip_a].skeleton != nclips[b.clip_b].skeleton) {
                errorOut = "blend \"" + b.id +
                           "\" clips must share the same skeleton";
                return false;
            }
            nblends[b.id] = b;
        }
        skeletons_ = std::move(nskeletons);
        clips_ = std::move(nclips);
        blends_ = std::move(nblends);
        errorOut.clear();
        return true;
    }

private:
    std::map<std::string, SkeletonSpec> skeletons_;
    std::map<std::string, ClipSpec> clips_;
    std::map<std::string, BlendSpec> blends_;
};

}  // namespace

std::unique_ptr<IAnimCore> create_anim_core() {
    return std::make_unique<AnimCore>();
}

}  // namespace animation
}  // namespace engine
