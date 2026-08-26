// IEntityArchetype — templates data-driven de entidade por KIND. Componente
// CORE do §1 item 13 ("unificar player, mobs, veículos, projéteis e objetos
// interativos no Scene/ECS público"): cada entidade é instanciada de um
// archetype — nome + kind + componentes (tipo + payload JSON opaco). O
// registro é a fonte única de "que entidade existe e com o quê"; o runtime
// de entidades (IEntityWorld) instancia a partir do archetype. Puro e
// testável headless (sem depender do runtime de entidades).
//
// JSON versionado all-or-nothing (kind desconhecido, nome vazio/duplicado,
// componente com tipo vazio ou JSON malformado → rejeita o doc inteiro).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace entity {

enum class EntityKind : std::uint8_t { Player, Mob, Vehicle, Projectile, Interactive };

const char* entity_kind_name(EntityKind kind);

struct ArchetypeComponent {
    std::string type;   // ex.: "health", "mob_ai", "physics"
    std::string json;   // payload opaco (deve ser JSON válido)
};

struct EntityArchetype {
    std::string name;
    EntityKind kind{ EntityKind::Mob };
    std::vector<ArchetypeComponent> components;
};

class IEntityArchetypeRegistry {
public:
    virtual ~IEntityArchetypeRegistry() = default;

    // All-or-nothing: nome vazio/duplicado → rejeita. Componente com tipo
    // vazio ou JSON malformado também rejeita (nunca registra parcial).
    virtual bool register_archetype(const EntityArchetype& archetype,
                                    std::string& errorOut) = 0;

    // Carrega um doc JSON versionado ({"version":1,"archetypes":[...]}).
    virtual bool load_from_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string to_json() const = 0;

    virtual const EntityArchetype* find(const std::string& name) const = 0;
    virtual std::vector<std::string> names() const = 0;  // ordem crescente
    virtual std::size_t count() const = 0;
    virtual void clear() = 0;
};

std::unique_ptr<IEntityArchetypeRegistry> create_entity_archetype_registry();

}  // namespace entity
}  // namespace engine
