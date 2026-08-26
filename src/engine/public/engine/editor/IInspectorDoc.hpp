#pragma once
// IInspectorDoc — contrato público do modelo do Inspector do editor
// (agente 2 §C, "integrar inspector/authoring").
//
// Descreve, de forma determinística, a estrutura do Inspector para uma
// entidade: quais componentes ela tem, agrupados pelos grupos semânticos que
// o painel usa (Aparência / Física / Jogabilidade / Animações), com um
// descritor de propriedade tipado para cada componente. O painel visual pode
// consumir este doc em vez de headers ad-hoc; o editor expõe o doc real da
// entidade selecionada via GET /inspector. SEM RNG/relógio/estado global.
// Self-contained (std apenas).

#include <memory>
#include <string>
#include <vector>

namespace engine::editor {

// Grupos semânticos do Inspector (ordem canônica do painel).
enum class InspectorGroup {
    Appearance,  // Aparência (mesh renderer, ...)
    Physics,     // Física (rigidbody, destruction, ...)
    Gameplay,    // Jogabilidade (weapon, vehicle, ragdoll, mission, dialogue, navigation, ...)
    Animation,   // Animação (animation, timeline, ik, retarget, ...)
};

// Tipo de uma propriedade editável (o shell decide o widget).
enum class PropertyType {
    Text,   // nome, strings
    Vec3,   // position/rotation/scale
    Float,  // valores numéricos
    Bool,   // liga/desliga
    Asset,  // seletor de asset (mesh picker)
};

// Um descritor de propriedade editável de um componente.
struct InspectorProperty {
    std::string name;      // chave estável (ex: "position")
    PropertyType type = PropertyType::Float;
};

// Um componente presente na entidade, com suas propriedades.
struct InspectorComponent {
    std::string name;  // chave estável (ex: "transform", "mesh_renderer")
    InspectorGroup group = InspectorGroup::Appearance;
    std::vector<InspectorProperty> properties;  // ordem de edição
};

// O documento completo do inspector para uma entidade.
struct InspectorDoc {
    std::string entity_name;  // nome da entidade (vazio se nenhuma)
    bool has_entity = false;  // false → "No Object Selected"
    std::vector<InspectorComponent> components;  // na ordem de exibição do painel
};

// Contrato do modelo do inspector.
struct IInspectorDoc {
    virtual ~IInspectorDoc() = default;

    // Constrói o doc para uma entidade com os flags de componentes presentes.
    // entity_name vazio + has_entity=false → doc vazio (nada selecionado).
    virtual InspectorDoc build(const std::string& entity_name, bool has_entity,
                               bool has_transform, bool has_mesh_renderer,
                               bool has_rigidbody, bool has_destruction,
                               bool has_weapon, bool has_vehicle,
                               bool has_ragdoll, bool has_animation,
                               bool has_timeline, bool has_ik,
                               bool has_retarget, bool has_mission,
                               bool has_dialogue, bool has_navigation) const = 0;

    // JSON determinístico do doc: {"entity","has_entity","components":[...]}.
    virtual std::string to_json(const InspectorDoc& doc) const = 0;
};

// Factory do adapter (implementada em src/engine/sdk/InspectorDoc.cpp).
std::unique_ptr<IInspectorDoc> create_inspector_doc();

}  // namespace engine::editor
