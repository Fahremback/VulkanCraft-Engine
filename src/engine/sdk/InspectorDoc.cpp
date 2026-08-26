// InspectorDoc — adapter do contrato engine/editor IInspectorDoc.
//
// Modelo determinístico do Inspector: os MESMOS grupos semânticos que o
// draw_inspector_panel() usa (Aparência → Física → Jogabilidade → Animação),
// com as propriedades editáveis de cada componente. O painel pode consumir
// este doc em vez de headers ad-hoc. Sem RNG/relógio/estado global.

#include "engine/editor/IInspectorDoc.hpp"

#include <sstream>

namespace engine::editor {

namespace {

const char* group_name(InspectorGroup g) {
    switch (g) {
        case InspectorGroup::Appearance: return "appearance";
        case InspectorGroup::Physics: return "physics";
        case InspectorGroup::Gameplay: return "gameplay";
        case InspectorGroup::Animation: return "animation";
    }
    return "appearance";
}

const char* type_name(PropertyType t) {
    switch (t) {
        case PropertyType::Text: return "text";
        case PropertyType::Vec3: return "vec3";
        case PropertyType::Float: return "float";
        case PropertyType::Bool: return "bool";
        case PropertyType::Asset: return "asset";
    }
    return "float";
}

class InspectorDocImpl : public IInspectorDoc {
public:
    InspectorDoc build(const std::string& entity_name, bool has_entity,
                       bool has_transform, bool has_mesh_renderer,
                       bool has_rigidbody, bool has_destruction,
                       bool has_weapon, bool has_vehicle,
                       bool has_ragdoll, bool has_animation,
                       bool has_timeline, bool has_ik,
                       bool has_retarget, bool has_mission,
                       bool has_dialogue, bool has_navigation) const override {
        InspectorDoc doc;
        doc.entity_name = entity_name;
        doc.has_entity = has_entity;
        if (!has_entity) {
            return doc;  // "No Object Selected"
        }
        if (has_transform) {
            doc.components.push_back(make_transform());
        }
        if (has_mesh_renderer) {
            doc.components.push_back(make_mesh_renderer());
        }
        if (has_rigidbody) {
            doc.components.push_back(
                make_simple("rigidbody", InspectorGroup::Physics, "mass", PropertyType::Float));
        }
        if (has_destruction) {
            doc.components.push_back(
                make_simple("destruction", InspectorGroup::Physics, "health", PropertyType::Float));
        }
        if (has_weapon) {
            doc.components.push_back(
                make_simple("weapon", InspectorGroup::Gameplay, "damage", PropertyType::Float));
        }
        if (has_vehicle) {
            doc.components.push_back(
                make_simple("vehicle", InspectorGroup::Gameplay, "speed", PropertyType::Float));
        }
        if (has_ragdoll) {
            doc.components.push_back(
                make_simple("ragdoll", InspectorGroup::Gameplay, "enabled", PropertyType::Bool));
        }
        if (has_mission) {
            doc.components.push_back(
                make_simple("mission", InspectorGroup::Gameplay, "objective", PropertyType::Text));
        }
        if (has_dialogue) {
            doc.components.push_back(
                make_simple("dialogue", InspectorGroup::Gameplay, "line", PropertyType::Text));
        }
        if (has_navigation) {
            doc.components.push_back(
                make_simple("navigation", InspectorGroup::Gameplay, "speed", PropertyType::Float));
        }
        if (has_animation) {
            doc.components.push_back(
                make_simple("animation", InspectorGroup::Animation, "clip", PropertyType::Asset));
        }
        if (has_timeline) {
            doc.components.push_back(
                make_simple("timeline", InspectorGroup::Animation, "time", PropertyType::Float));
        }
        if (has_ik) {
            doc.components.push_back(
                make_simple("ik", InspectorGroup::Animation, "enabled", PropertyType::Bool));
        }
        if (has_retarget) {
            doc.components.push_back(
                make_simple("retarget", InspectorGroup::Animation, "source", PropertyType::Asset));
        }
        return doc;
    }

    std::string to_json(const InspectorDoc& doc) const override {
        std::ostringstream out;
        out << "{\"entity\":\"" << doc.entity_name
            << "\",\"has_entity\":" << (doc.has_entity ? "true" : "false")
            << ",\"components\":[";
        for (std::size_t i = 0; i < doc.components.size(); ++i) {
            const InspectorComponent& c = doc.components[i];
            if (i > 0) out << ",";
            out << "{\"name\":\"" << c.name << "\",\"group\":\""
                << group_name(c.group) << "\",\"properties\":[";
            for (std::size_t j = 0; j < c.properties.size(); ++j) {
                if (j > 0) out << ",";
                out << "{\"name\":\"" << c.properties[j].name << "\",\"type\":\""
                    << type_name(c.properties[j].type) << "\"}";
            }
            out << "]}";
        }
        out << "]}";
        return out.str();
    }

private:
    static InspectorComponent make_transform() {
        InspectorComponent c;
        c.name = "transform";
        c.group = InspectorGroup::Appearance;
        c.properties = {
            {"position", PropertyType::Vec3},
            {"rotation", PropertyType::Vec3},
            {"scale", PropertyType::Vec3},
        };
        return c;
    }

    static InspectorComponent make_mesh_renderer() {
        InspectorComponent c;
        c.name = "mesh_renderer";
        c.group = InspectorGroup::Appearance;
        c.properties = {
            {"mesh", PropertyType::Asset},
            {"cast_shadow", PropertyType::Bool},
        };
        return c;
    }

    static InspectorComponent make_simple(const char* name, InspectorGroup g,
                                          const char* prop, PropertyType t) {
        InspectorComponent c;
        c.name = name;
        c.group = g;
        c.properties = {{prop, t}};
        return c;
    }
};

}  // namespace

std::unique_ptr<IInspectorDoc> create_inspector_doc() {
    return std::make_unique<InspectorDocImpl>();
}

}  // namespace engine::editor
