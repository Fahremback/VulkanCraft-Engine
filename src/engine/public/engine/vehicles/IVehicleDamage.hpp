// IVehicleDamage — modelo determinístico de DANO por peça de veículo.
// Componente CORE do §6 item 57 ("integrar dano/deformação/fragmentação a
// veículos ... sem lógica específica do jogo"): o veículo é um conjunto de
// peças (cada uma com saúde máxima e opcionalmente DESTACÁVEL); `apply_damage`
// reduz a saúde (clamp em 0), destrói a peça em 0 e a DESTACA quando
// destacável e a saúde cai abaixo do limiar (fração da máxima). O modelo é
// puro (sem física); o runtime físico aplica o destacamento de fato.
// `to_json` serializa config + saúde (persistência/telemetria).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace vehicles {

struct VehiclePartSpec {
    std::string name;
    float maxHealth{ 100.0f };     // > 0
    bool detachable{ false };
    float detachThreshold{ 0.0f }; // fração da saúde (0..1) abaixo da qual destaca
};

struct VehicleDamageResult {
    std::vector<std::string> newlyDestroyed;  // peças que zeraram NESTE apply
    std::vector<std::string> newlyDetached;   // peças que destacaram NESTE apply
    float totalDamage{ 0.0f };                // dano realmente aplicado
};

class IVehicleDamage {
public:
    virtual ~IVehicleDamage() = default;

    // All-or-nothing: nome vazio/duplicado, maxHealth <= 0 ou não-finita,
    // detachThreshold fora de [0,1] → rejeita a lista inteira.
    virtual bool configure(const std::vector<VehiclePartSpec>& parts,
                           std::string& errorOut) = 0;

    // Aplica dano a uma peça. Peça desconhecida → resultado vazio (0 dano).
    // Destruída/destacada NÃO recebe mais dano (resultado vazio).
    virtual VehicleDamageResult apply_damage(const std::string& part,
                                             float amount) = 0;

    virtual float health(const std::string& part) const = 0;  // -1 se desconhecida
    virtual bool is_destroyed(const std::string& part) const = 0;
    virtual bool is_detached(const std::string& part) const = 0;

    virtual std::vector<std::string> destroyed_parts() const = 0;  // ordem crescente
    virtual std::vector<std::string> detached_parts() const = 0;   // ordem crescente

    virtual void repair_all() = 0;

    // Estado completo (config + saúde) em JSON; load all-or-nothing.
    virtual std::string to_json() const = 0;
    virtual bool load_from_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
};

std::unique_ptr<IVehicleDamage> create_vehicle_damage();

}  // namespace vehicles
}  // namespace engine
