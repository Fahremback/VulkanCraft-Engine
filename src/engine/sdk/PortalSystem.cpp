// PortalSystem.cpp — the only TU implementing the public portal system
// contract (Agente 4 §6 item 66 CORE): portal links between worlds with
// deterministic find (nearest) and resolve (offset + rotation). Pure std +
// RegistryJson; no world runtime dependency — the caller executes the
// actual entity transfer. Bit-level finite checks (/fp:fast guard #79).

#include "engine/world/IPortalSystem.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace engine {
namespace world {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

bool finite_vec3(const glm::vec3& v) {
    return finite_float(v.x) && finite_float(v.y) && finite_float(v.z);
}

bool finite_quat(const glm::quat& q) {
    return finite_float(q.x) && finite_float(q.y) &&
           finite_float(q.z) && finite_float(q.w);
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
        value->array.size() != 3) return false;
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
        value->array.size() != 4) return false;
    glm::quat result{1.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; ++i) {
        if (value->array[i].kind != sdk::JsonValue::Kind::Number) return false;
        result[i] = static_cast<float>(value->array[i].number);
    }
    if (!finite_quat(result)) return false;
    out = result;
    return true;
}

bool check_links(const std::vector<PortalLink>& links, std::string& errorOut) {
    for (std::size_t i = 0; i < links.size(); ++i) {
        const PortalLink& link = links[i];
        if (link.name.empty()) {
            errorOut = "portal system: link at index " + std::to_string(i) +
                       " has an empty name";
            return false;
        }
        if (link.fromWorld.empty() || link.toWorld.empty()) {
            errorOut = "portal system: link '" + link.name +
                       "' needs fromWorld and toWorld";
            return false;
        }
        if (link.fromWorld == link.toWorld) {
            errorOut = "portal system: link '" + link.name +
                       "' connects a world to itself";
            return false;
        }
        if (!finite_vec3(link.fromCenter) || !finite_vec3(link.toCenter) ||
            !finite_quat(link.rotation)) {
            errorOut = "portal system: link '" + link.name +
                       "' has non-finite centers/rotation";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (links[j].name == link.name) {
                errorOut = "portal system: duplicate link name '" + link.name + "'";
                return false;
            }
        }
    }
    return true;
}

class PortalSystem final : public IPortalSystem {
public:
    PortalSystem() = default;

    bool configure(const std::vector<PortalLink>& links,
                   std::string& errorOut) override {
        if (!check_links(links, errorOut)) return false;
        links_ = links;
        return true;
    }

    bool load_from_json(const std::string& jsonText,
                        std::string& errorOut) override {
        sdk::JsonValue root;
        if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
            if (errorOut.empty()) errorOut = "portal system: root must be an object";
            return false;
        }
        const int version = static_cast<int>(sdk::json_number(root, "version", 1));
        if (version != 1) {
            errorOut = "portal system: unsupported version " + std::to_string(version);
            return false;
        }
        const sdk::JsonValue* portalsValue = root.field("portals");
        if (portalsValue == nullptr ||
            portalsValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "portal system: portals must be an array";
            return false;
        }
        std::vector<PortalLink> parsed;
        parsed.reserve(portalsValue->array.size());
        for (const sdk::JsonValue& portalValue : portalsValue->array) {
            if (!portalValue.is_object()) {
                errorOut = "portal system: each portal must be an object";
                return false;
            }
            PortalLink link;
            link.name = sdk::json_string(portalValue, "name", "");
            link.fromWorld = sdk::json_string(portalValue, "fromWorld", "");
            link.toWorld = sdk::json_string(portalValue, "toWorld", "");
            if (!read_vec3(portalValue, "fromCenter", link.fromCenter)) {
                errorOut = "portal system: fromCenter must be a finite [x,y,z] array";
                return false;
            }
            if (!read_vec3(portalValue, "toCenter", link.toCenter)) {
                errorOut = "portal system: toCenter must be a finite [x,y,z] array";
                return false;
            }
            if (!read_quat(portalValue, "rotation", link.rotation)) {
                errorOut = "portal system: rotation must be a finite [x,y,z,w] array";
                return false;
            }
            parsed.push_back(std::move(link));
        }
        if (!check_links(parsed, errorOut)) return false;
        links_ = std::move(parsed);
        return true;
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"version\":1,\"portals\":[";
        for (std::size_t i = 0; i < links_.size(); ++i) {
            if (i != 0) out << ",";
            const PortalLink& link = links_[i];
            out << "{\"name\":\"" << json_escape(link.name) << "\",\"fromWorld\":\""
                << json_escape(link.fromWorld) << "\",\"toWorld\":\""
                << json_escape(link.toWorld) << "\",\"fromCenter\":"
                << emit_vec3(link.fromCenter) << ",\"toCenter\":"
                << emit_vec3(link.toCenter) << ",\"rotation\":"
                << emit_quat(link.rotation) << "}";
        }
        out << "]}";
        return out.str();
    }

    bool find_portal(const std::string& world, float x, float z, float radius,
                     PortalLink& out) const override {
        const PortalLink* best = nullptr;
        float bestDist = 0.0f;
        for (const PortalLink& link : links_) {
            if (link.fromWorld != world) continue;
            const float dx = link.fromCenter.x - x;
            const float dz = link.fromCenter.z - z;
            const float dist = dx * dx + dz * dz;
            if (dist > radius * radius) continue;
            if (best == nullptr || dist < bestDist ||
                (dist == bestDist && link.name < best->name)) {
                best = &link;
                bestDist = dist;
            }
        }
        if (best == nullptr) return false;
        out = *best;
        return true;
    }

    bool resolve(const std::string& name, const glm::vec3& entry,
                 glm::vec3& exitPosition) const override {
        for (const PortalLink& link : links_) {
            if (link.name != name) continue;
            const glm::vec3 offset = entry - link.fromCenter;
            const glm::vec3 rotated = link.rotation * offset;
            exitPosition = link.toCenter + rotated;
            return true;
        }
        return false;
    }

    std::vector<PortalLink> links() const override { return links_; }
    std::size_t link_count() const override { return links_.size(); }
    void reset() override { links_.clear(); }

private:
    std::vector<PortalLink> links_;
};

}  // namespace

std::unique_ptr<IPortalSystem> create_portal_system() {
    return std::make_unique<PortalSystem>();
}

}  // namespace world
}  // namespace engine
