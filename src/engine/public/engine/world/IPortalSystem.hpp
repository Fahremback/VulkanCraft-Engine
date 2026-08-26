// IPortalSystem — definição e RESOLUÇÃO de portais entre mundos. Componente
// CORE do §6 item 66 ("integrar WorldManager a portais, transferência de
// entidades e coordenadas entre mundos"): o WorldManager (AGENT-3) já
// transfere entidades entre mundos; este contrato adiciona a camada de
// COORDENADAS — onde cada portal conecta um ponto de entrada (mundo A) a um
// ponto de saída (mundo B) com rotação opcional, e `resolve` calcula a
// posição de saída a partir da de entrada (offset relativo ao centro de
// entrada, rotacionado, somado ao centro de saída). Puro, sem depender do
// runtime de mundo — o chamador (WorldManager/IEntityWorld) executa a
// transferência de fato.
//
// Links data-driven: configure(vector<PortalLink>) all-or-nothing (nome
// duplicado, from == to, valores não-finitos → rejeita a lista inteira) e
// JSON versionado all-or-nothing. Sem RNG, sem estado global.

#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace world {

// Um portal: conecta `fromWorld` (em `fromCenter`) a `toWorld` (em
// `toCenter`), aplicando `rotation` ao offset de entrada.
struct PortalLink {
    std::string name;
    std::string fromWorld;
    std::string toWorld;
    glm::vec3 fromCenter{ 0.0f };
    glm::vec3 toCenter{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
};

class IPortalSystem {
public:
    virtual ~IPortalSystem() = default;

    // Substitui a lista de links. All-or-nothing: nome vazio/duplicado,
    // from == to (mesmo mundo), nome de mundo vazio, valores não-finitos →
    // rejeita a lista INTEIRA e mantém a anterior.
    virtual bool configure(const std::vector<PortalLink>& links,
                           std::string& errorOut) = 0;

    // Carrega links de JSON versionado ({"version":1,"portals":[...]}),
    // all-or-nothing igual ao configure.
    virtual bool load_from_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string to_json() const = 0;

    // O portal cujo `fromWorld` casa e cuja distância horizontal ao ponto
    // (x, z) em fromWorld é <= radius; empate = menor distância, depois nome.
    // Retorna false se nenhum (all-or-nothing).
    virtual bool find_portal(const std::string& world, float x, float z,
                             float radius, PortalLink& out) const = 0;

    // Resolve a posição de saída: offset = entrada - fromCenter, rotacionado
    // por `rotation`, + toCenter. Portal desconhecido → false.
    virtual bool resolve(const std::string& name, const glm::vec3& entry,
                         glm::vec3& exitPosition) const = 0;

    virtual std::vector<PortalLink> links() const = 0;
    virtual std::size_t link_count() const = 0;
    virtual void reset() = 0;
};

std::unique_ptr<IPortalSystem> create_portal_system();

}  // namespace world
}  // namespace engine
