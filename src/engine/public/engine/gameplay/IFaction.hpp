#pragma once
// IFaction — contrato público de facções/equipes (agente 4 §1 item 16 "equipe").
//
// Modelo de relação entre equipes, puro e determinístico: SEM RNG, SEM relógio
// de parede, SEM estado global. A mesma spec produz as mesmas relações
// bit-exatas entre instâncias. Complementa `engine/ai/IPerception.hpp` (o
// `hostile` ali é um flag por estímulo; aqui a relação é por EQUIPE, simétrica
// e data-driven). JSON versionado all-or-nothing bit-exact.
//
// Relações: Friendly / Hostile / Neutral (default). `set_relation` é simétrico
// (a↔b atualiza nos dois sentidos) — uma equipe nunca é "hostil para b" sem b
// ser "hostil para a".

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine::gameplay {

enum class FactionRelation {
    Friendly,
    Hostile,
    Neutral,
};

struct FactionSpec {
    std::vector<std::string> teams;                      // ids únicos não-vazios
    struct Relation {
        std::string a;
        std::string b;
        FactionRelation relation = FactionRelation::Neutral;
    };
    std::vector<Relation> relations;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact
};

// Modelo de facções (relações simétricas determinísticas).
class IFaction {
public:
    virtual ~IFaction() = default;

    // Aplica a spec (all-or-nothing via FactionSpec::validate).
    virtual bool configure(const FactionSpec& spec, std::string& errorOut) = 0;

    // Registra uma equipe nova (id único não-vazio; duplicado recusa sem mutar).
    virtual bool register_team(const std::string& team, std::string& errorOut) = 0;

    // Define a relação simétrica entre a e b (ambos os sentidos).
    virtual bool set_relation(const std::string& a, const std::string& b,
                              FactionRelation relation,
                              std::string& errorOut) = 0;

    // Relação a→b (simétrica; desconhecido/ausente = Neutral).
    virtual FactionRelation relation(const std::string& a,
                                     const std::string& b) const = 0;

    virtual bool is_hostile(const std::string& a, const std::string& b) const = 0;
    virtual bool is_friendly(const std::string& a, const std::string& b) const = 0;

    // Ids das equipes registradas, ordenados (determinístico).
    virtual std::vector<std::string> teams() const = 0;

    // Estado (relações) serializado bit-exact / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IFaction).
std::unique_ptr<IFaction> create_faction();

}  // namespace engine::gameplay