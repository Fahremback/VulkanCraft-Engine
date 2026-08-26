#pragma once
// INetworkInterest — interest management determinístico (relevância de rede)
// para o domínio `engine/networking/` (§6 item 4 — "interest management" da
// rede pública).
//
// Núcleo headless da relevância: cada observador declara um raio de interesse
// e a sessão calcula QUAIS entidades são relevantes para QUEM — o subconjunto
// que o transporte deve enviar. O contrato NÃO conhece a rede nem a simulação:
// as posições são pontos 2D/3D opacos (coordenadas fornecidas pelo chamador),
// e o resultado é um conjunto determinístico por (observador, tick). Mesmo
// espírito do INetworkReplication: dados opacos, ordem determinística.
//
// Self-contained (std only), headless, determinístico. Persistência JSON
// bit-exact e all-or-nothing (load só comita se TODOS os registros forem
// válidos — id duplicado, raio inválido ou posição ausente rejeita o
// documento inteiro e deixa o estado anterior intacto).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::networking {

// Posição de uma entidade ou observador no mundo (coordenadas fornecidas
// pelo chamador — o contrato nunca as interpreta além de distância).
struct NetworkPosition {
    double x{ 0.0 };
    double y{ 0.0 };
    double z{ 0.0 };
};

// Um observador com raio de interesse.
struct InterestObserver {
    std::uint64_t observer_id{ 0 };
    NetworkPosition position;
    double radius{ 0.0 };          // raio de interesse (>= 0)
    bool always_relevant{ false }; // recebe tudo, ignorando raio
};

// Uma entidade com posição corrente.
struct InterestEntity {
    std::uint64_t entity_id{ 0 };
    NetworkPosition position;
};

// Resultado do cálculo de relevância para um observador: ids das entidades
// relevantes, em ordem crescente.
struct InterestResult {
    std::uint64_t observer_id{ 0 };
    std::vector<std::uint64_t> entity_ids;   // ordenados crescentemente
};

class INetworkInterest {
public:
    virtual ~INetworkInterest() = default;

    // Identificador fixo da sessão de interesse.
    virtual const std::string& session_id() const = 0;

    // Registra/atualiza um observador. Raio < 0 → false e NADA muda
    // (all-or-nothing).
    virtual bool set_observer(const InterestObserver& observer, std::string& errorOut) = 0;

    // Remove um observador. Ausente = no-op.
    virtual void remove_observer(std::uint64_t observer_id) = 0;

    // Registra/atualiza a posição de uma entidade. Sempre ok.
    virtual void set_entity(const InterestEntity& entity) = 0;

    // Remove uma entidade. Ausente = no-op.
    virtual void remove_entity(std::uint64_t entity_id) = 0;

    // Calcula a relevância de TODOS os observadores. Uma entidade é relevante
    // para um observador se `always_relevant`, ou se a distância euclidiana
    // entre as posições <= radius. Entidades desconhecidas (sem posição) nunca
    // são relevantes. Resultados ordenados por observer_id; entity_ids em
    // ordem crescente. Determinístico.
    virtual std::vector<InterestResult> compute() const = 0;

    // Observadores registrados, em ordem crescente de id.
    virtual std::vector<InterestObserver> observers() const = 0;

    // Entidades registradas, em ordem crescente de id.
    virtual std::vector<InterestEntity> entities() const = 0;

    // Descarta tudo (nova sessão). Sempre ok.
    virtual bool reset(std::string& errorOut) = 0;

    // --- Persistência (bit-exact, all-or-nothing) ---
    virtual bool load_from_json(const std::string& json, std::string& errorOut) = 0;
    virtual std::string serialize_state() const = 0;
};

// Cria uma sessão de interesse. `sessionId` deve ser não-vazio (all-or-nothing).
std::unique_ptr<INetworkInterest> create_network_interest(const std::string& sessionId,
                                                          std::string& errorOut);

}  // namespace engine::networking
