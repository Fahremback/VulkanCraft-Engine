#pragma once
// INetworkDiscovery — service discovery determinístico (registro e saúde de
// serviços) para o domínio `engine/networking/` (§6 item 4 — "discovery" da
// rede pública).
//
// Núcleo headless do discovery: serviços se registram com tipo + endpoint, o
// contrato mantém o estado de saúde (healthy = sem falhas consecutivas) e
// resolve por tipo — o subconjunto de serviços SAUDÁVEIS que o transporte usa
// para conectar. O contrato NÃO conhece a rede: endpoints são strings opacas
// ("tcp://host:port", "localhost:8080") fornecidas pelo chamador, e a saúde é
// alimentada por probes externos via report_health. Mesmo espírito do
// INetworkReplication/INetworkRpc: dados opacos, ordem determinística.
//
// Self-contained (std only), headless, determinístico. Persistência JSON
// bit-exact e all-or-nothing (load só comita se TODOS os registros forem
// válidos — id duplicado, tipo/endpoint vazio ou campo desconhecido rejeita o
// documento inteiro e deixa o estado anterior intacto).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::networking {

// Um serviço registrado no discovery.
struct DiscoveryService {
    std::uint64_t service_id{ 0 };
    std::string type;             // tipo opaco: "gameplay", "replication", ...
    std::string endpoint;         // endpoint opaco: "tcp://host:port", ...
    std::uint64_t consecutive_failures{ 0 };  // falhas consecutivas do último probe
    bool healthy{ false };        // healthy == (consecutive_failures == 0)
};

class INetworkDiscovery {
public:
    virtual ~INetworkDiscovery() = default;

    // Identificador fixo da sessão de discovery.
    virtual const std::string& session_id() const = 0;

    // Registra/atualiza um serviço. Tipo ou endpoint vazio → false e NADA muda
    // (all-or-nothing). Atualização sobrescreve sem duplicar.
    virtual bool register_service(const DiscoveryService& service, std::string& errorOut) = 0;

    // Remove um serviço. Ausente = no-op.
    virtual void unregister_service(std::uint64_t service_id) = 0;

    // Alimenta a saúde com o resultado de um probe. `ok=true` zera o contador
    // de falhas; `ok=false` incrementa. healthy é derivado: só é true quando
    // o contador de falhas consecutivas é 0. Serviço desconhecido = no-op.
    virtual void report_health(std::uint64_t service_id, bool ok) = 0;

    // Resolve os serviços de um tipo — SOMENTE os saudáveis, ordenados por id
    // crescente. Tipo desconhecido → vetor vazio.
    virtual std::vector<DiscoveryService> resolve(const std::string& type) const = 0;

    // Todos os serviços registrados, em ordem crescente de id.
    virtual std::vector<DiscoveryService> services() const = 0;

    // Descarta tudo (nova sessão). Sempre ok.
    virtual bool reset(std::string& errorOut) = 0;

    // --- Persistência (bit-exact, all-or-nothing) ---
    virtual bool load_from_json(const std::string& json, std::string& errorOut) = 0;
    virtual std::string serialize_state() const = 0;
};

// Cria uma sessão de discovery. `sessionId` deve ser não-vazio (all-or-nothing).
std::unique_ptr<INetworkDiscovery> create_network_discovery(const std::string& sessionId,
                                                            std::string& errorOut);

}  // namespace engine::networking
