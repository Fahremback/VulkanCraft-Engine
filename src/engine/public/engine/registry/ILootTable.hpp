#pragma once
// ILootTable — contrato público de loot tables data-driven
// (agente 4 §1 item 17 — sobre os contratos do Agente 3).
//
// Rolagem de drops determinística e headless sobre ids de item namespaced
// (ex.: "vulkancraft:iron_ingot") — SEM acoplar ao ItemRegistry em si: a
// tabela trabalha com strings e o caller cruza com o catálogo real via
// `validate_items`. Puro e determinístico: SEM RNG de plataforma, SEM relógio,
// SEM estado global; a mesma spec + seed produzem o MESMO resultado bit-exato
// em qualquer instância/plataforma (RNG = splitmix64, inteiros puros). JSON
// versionado all-or-nothing bit-exact.
//
// Modelo:
//   - entry: item id + `weight` (seleção ponderada), `count_min`/`count_max`
//     (range do stack), `chance` (probabilidade por roll, [0,1]).
//   - roll(seed): 1) número de rolls = rolls_min..rolls_max (via RNG);
//     2) cada roll seleciona uma entry por peso; 3) se chance < 1, o roll pode
//     ser perdido; 4) count sorteado no range; 5) saída MERGEADA por item e
//     ORDENADA por id (determinístico).
//   - validate_items(known): cruza com o catálogo do mundo; devolve os ids
//     desconhecidos em ordem sorted (vazio = consistente).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::registry {

struct LootEntry {
    std::string item;      // id namespaced (ex.: "vulkancraft:iron_ingot")
    double weight = 1.0;   // peso na seleção (finita, > 0)
    int count_min = 1;     // >= 0
    int count_max = 1;     // >= count_min
    double chance = 1.0;   // [0,1] probabilidade por roll
};

struct LootTableSpec {
    std::string id;
    std::vector<LootEntry> entries;
    int rolls_min = 1;  // >= 0
    int rolls_max = 1;  // >= rolls_min

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

struct LootRoll {
    std::string item;
    int count = 0;
};

// Loot table (rolagem determinística por seed).
class ILootTable {
public:
    virtual ~ILootTable() = default;

    // Aplica a spec (all-or-nothing via LootTableSpec::validate).
    virtual bool configure(const LootTableSpec& spec, std::string& errorOut) = 0;

    // Rola a tabela com a seed (determinístico cross-instance). Saída mergeada
    // por item e ordenada por id (id ASC).
    virtual std::vector<LootRoll> roll(std::uint64_t seed) = 0;

    // Cruza os itens da tabela com um catálogo conhecido; devolve os
    // desconhecidos em ordem sorted (vazio = consistente).
    virtual std::vector<std::string> validate_items(
        const std::vector<std::string>& known) const = 0;

    // Ids distintos da tabela (ordem sorted) — o catálogo a validar.
    virtual std::vector<std::string> items() const = 0;

    // Spec serializada bit-exact / restaurada all-or-nothing (a tabela não
    // tem estado evolutivo — o roll é puro em relação à seed).
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando ILootTable).
std::unique_ptr<ILootTable> create_loot_table();

}  // namespace engine::registry
