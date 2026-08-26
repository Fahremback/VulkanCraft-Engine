#pragma once
// IEquipment — contrato público de equipamento/slots data-driven
// (agente 4 §1 item 17 — parte "equipamentos"; fecha o item).
//
// Slots de equipamento determinísticos e headless sobre ids/tags de item
// namespaced — SEM acoplar ao ItemRegistry: o caller informa as TAGS do item
// no equip (ex.: ["vulkancraft:armor", "vulkancraft:iron"]) e o slot valida
// contra as tags permitidas da categoria. Puro e determinístico: SEM RNG, SEM
// relógio, SEM estado global; a mesma spec + sequência de equip/unequip
// produzem o mesmo estado bit-exato entre instâncias. JSON versionado
// all-or-nothing bit-exact.
//
// Modelo:
//   - category: slot (ex.: head/chest/legs/feet/hand/offhand) + tags
//     permitidas (vazio = aceita QUALQUER item).
//   - equip(category, item, tags): cabe se algum tag do item pertence às tags
//     da categoria (ou a categoria não tem restrição); substitui o atual
//     (slot único por categoria); recusa all-or-nothing.
//   - unequip(category): esvazia o slot.
//   - equipped/items: consulta determinística (items sorted por categoria).

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine::registry {

struct EquipmentCategory {
    std::string id;
    // Tags que cabem neste slot; vazio = aceita qualquer item.
    std::vector<std::string> tags;
};

struct EquipmentSpec {
    std::vector<EquipmentCategory> categories;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact
};

// Slots de equipamento (validação e estado determinístico).
class IEquipment {
public:
    virtual ~IEquipment() = default;

    // Aplica a spec (all-or-nothing via EquipmentSpec::validate).
    virtual bool configure(const EquipmentSpec& spec, std::string& errorOut) = 0;

    // Equipa `item` (com suas `itemTags`) no slot `category`. Cabe se a
    // categoria não tem restrição ou algum tag do item está na allowlist;
    // substitui o item anterior do slot. Recusa all-or-nothing.
    virtual bool equip(const std::string& category, const std::string& item,
                       const std::vector<std::string>& itemTags,
                       std::string& errorOut) = 0;

    // Esvazia o slot (no-op se já vazio; recusa categoria desconhecida).
    virtual bool unequip(const std::string& category, std::string& errorOut) = 0;

    // Item equipado no slot ("" = vazio); recusa categoria desconhecida.
    virtual std::string equipped(const std::string& category,
                                 std::string& errorOut) const = 0;

    // Pares {categoria, item} ordenados por categoria (só slots ocupados).
    virtual std::vector<std::pair<std::string, std::string>> items() const = 0;

    // Categorias da spec (ordem de declaração).
    virtual std::vector<std::string> categories() const = 0;

    // Estado (itens por slot) serializado bit-exact / restaurado all-or-
    // nothing (categoria desconhecida recusa sem mutar).
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IEquipment).
std::unique_ptr<IEquipment> create_equipment();

}  // namespace engine::registry
