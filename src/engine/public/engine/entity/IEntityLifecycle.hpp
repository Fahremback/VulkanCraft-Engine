// IEntityLifecycle — máquina de estados do ciclo de vida de entidades com
// POOLING e rastreio de persistência. Componente CORE do §1 item 14
// ("ciclo completo spawn, despawn, sleep, wake, pooling, persistência e
// transferência entre regiões/mundos"): o rastreio de estado é puro e
// testável headless; o runtime de entidades (IEntityWorld) executa as
// transições de fato e usa `dirty()`/checkpoint p/ persistir.
//
// Estados: Despawned → spawn() → Active → sleep() → Sleeping → wake() →
// Active → despawn() → Despawned. O pool limita o nº de entidades vivas
// (spawn além da capacidade recusa, sem mutar). Transições inválidas
// recusam. Persistência: `set_persistent` marca a entidade; `dirty()` lista
// (em ordem crescente) as registradas com mudança de estado/flag desde o
// último `mark_checkpoint()`. Estado completo serializável (JSON) p/
// persistência e transferência entre mundos.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace entity {

enum class LifecycleState : std::uint8_t { Despawned, Active, Sleeping };

const char* lifecycle_state_name(LifecycleState state);

class IEntityLifecycle {
public:
    virtual ~IEntityLifecycle() = default;

    // All-or-nothing: poolCapacity == 0 recusa. Substitui (limpa) o rastreador.
    virtual bool configure(std::size_t poolCapacity, std::string& errorOut) = 0;

    // Registra a entidade (estado Despawned, não-persistente). All-or-nothing:
    // id duplicado recusa. Registrar conta como mudança (dirty até checkpoint).
    virtual bool register_entity(std::uint64_t entityId, std::string& errorOut) = 0;

    // Despawned → Active. Requer um slot livre no pool; pool cheio recusa.
    virtual bool spawn(std::uint64_t entityId) = 0;

    // Active → Sleeping.
    virtual bool sleep(std::uint64_t entityId) = 0;

    // Sleeping → Active.
    virtual bool wake(std::uint64_t entityId) = 0;

    // Active/Sleeping → Despawned (libera o slot do pool).
    virtual bool despawn(std::uint64_t entityId) = 0;

    virtual LifecycleState state(std::uint64_t entityId) const = 0;
    virtual bool is_registered(std::uint64_t entityId) const = 0;

    // Entidades no estado, em ordem crescente de id.
    virtual std::vector<std::uint64_t> by_state(LifecycleState state) const = 0;

    virtual std::size_t pool_used() const = 0;
    virtual std::size_t pool_capacity() const = 0;

    // Persistência: flag por entidade (transferência/persistência de mundo).
    virtual bool set_persistent(std::uint64_t entityId, bool persistent) = 0;
    virtual bool is_persistent(std::uint64_t entityId) const = 0;

    // Mudou (estado ou flag) desde o último checkpoint — ordem crescente.
    virtual std::vector<std::uint64_t> dirty() const = 0;
    virtual void mark_checkpoint() = 0;

    // Estado completo em JSON p/ persistência/transferência; load all-or-
    // nothing (id duplicado/estado desconhecido → rejeita o doc inteiro).
    virtual std::string to_json() const = 0;
    virtual bool load_from_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
};

std::unique_ptr<IEntityLifecycle> create_entity_lifecycle();

}  // namespace entity
}  // namespace engine
