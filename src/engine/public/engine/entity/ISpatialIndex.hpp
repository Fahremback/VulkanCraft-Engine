// ISpatialIndex — partição espacial de entidades por célula uniforme.
// Componente CORE do §1 item 15 ("integrar entidades à partição espacial,
// chunks, relevância de rede e budgets de simulação"): o índice insere
// entidades (id + AABB), remove/move e responde consultas de AABB e ponto —
// com resultados DETERMINÍSTICOS (ids em ordem crescente). O chamador
// (mundo/chunks/relevância de rede) usa os candidatos para o teste exato;
// este contrato é a partição em si, pura e testável headless.
//
// `configure(cellSize)` all-or-nothing (cellSize <= 0 ou não-finito recusa).
// Sem RNG, sem estado global.

#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace entity {

struct SpatialBounds {
    glm::vec3 min{ 0.0f };
    glm::vec3 max{ 0.0f };
};

class ISpatialIndex {
public:
    virtual ~ISpatialIndex() = default;

    // All-or-nothing: cellSize <= 0 ou não-finito recusa (estado anterior
    // preservado). Substitui o índice (limpa).
    virtual bool configure(float cellSize, std::string& errorOut) = 0;

    // Insere a entidade. All-or-nothing: id duplicado ou bounds inválidas
    // (min > max em alguma componente, não-finitas) recusam.
    virtual bool insert(std::uint64_t entityId, const SpatialBounds& bounds,
                        std::string& errorOut) = 0;

    // Remove; false para id desconhecido.
    virtual bool remove(std::uint64_t entityId) = 0;

    // Move (remove + reinsere com as novas bounds); false se desconhecido.
    virtual bool move(std::uint64_t entityId, const SpatialBounds& newBounds) = 0;

    // Candidatos cuja AABB intersecta a consulta, em ordem crescente de id.
    virtual std::vector<std::uint64_t> query_aabb(
        const SpatialBounds& query) const = 0;

    // Candidatos cuja AABB contém o ponto, em ordem crescente de id.
    virtual std::vector<std::uint64_t> query_point(float x, float y,
                                                   float z) const = 0;

    virtual std::size_t count() const = 0;
    virtual void clear() = 0;
};

std::unique_ptr<ISpatialIndex> create_spatial_index();

}  // namespace entity
}  // namespace engine
