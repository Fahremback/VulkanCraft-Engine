// IHilbertCellIndex.hpp
//
// PUBLIC seam para indexação espacial hierárquica estilo S2: células
// quadradas em níveis (quadtree) ordenadas pela curva de Hilbert. É a
// contraparte headless/determinística da geometria S2 (s2geometry) —
// implementada do zero no SDK, sem abseil/SWIG/OpenSSL, com a mesma
// propriedade central: células vizinhas na curva tendem a ser vizinhas no
// espaço, o que dá localidade para streaming de planetas/terreno em escala
// mundial, particionamento de regiões e buckets de rede.
//
// Codificação de cell id (estilo S2, sem face):
//   cellId = (1 << (2*level)) | (hilbertIndex << 1) | 1
//   - o bit mais significativo na posição 2*level marca o nível;
//   - hilbertIndex (xy->d) ocupa os bits [1, 2*level);
//   - o bit 0 é o "leaf marker" (S2).
//   Propriedades: cell_level = posição do MSB / 2; parent = descarta os 2
//   bits de menor ordem do nível; children = (d<<2)|k para k em 0..3.
// Níveis suportados: 0..30 (uma célula de nível 30 cobre 1 unidade de um
// grid 2^30 x 2^30 — precisão planetária).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace world {

// Configuração do índice (all-or-nothing: valores fora do range são
// recusados com diagnóstico, nunca clampeados).
struct HilbertCellConfig {
    std::uint32_t maxLevel{ 30 };   // nível máximo suportado [1, 30]
    std::uint64_t maxCoverCells{ 4096 };  // cap de células por cover [1, 1<<20]

    bool valid(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;
};

class IHilbertCellIndex {
public:
    virtual ~IHilbertCellIndex() = default;

    virtual bool configure(const HilbertCellConfig& config,
                           std::string& errorOut) = 0;
    virtual const HilbertCellConfig& config() const noexcept = 0;

    // cell id canônica da célula (x, y) no nível dado. (x, y) devem estar em
    // [0, 2^level). cell_id e cell_position são inversas bit-exact.
    virtual std::uint64_t cell_id(int x, int y, int level,
                                  std::string& errorOut) const = 0;

    // Decodifica um cell id: (x, y) na grade do seu nível.
    virtual bool cell_position(std::uint64_t cellId, int& x, int& y,
                               std::string& errorOut) const = 0;

    // Nível de um cell id (posição do MSB / 2). 0 para o domínio inteiro.
    virtual int cell_level(std::uint64_t cellId) const noexcept = 0;

    // Célula pai (nível - 1). Nível 0 é a raiz (pai dela mesma).
    virtual std::uint64_t parent_cell(std::uint64_t cellId) const noexcept = 0;

    // As 4 células filhas (nível + 1), em ordem de curva (k = 0..3).
    virtual std::vector<std::uint64_t> children_cells(
        std::uint64_t cellId) const noexcept = 0;

    // Cobertura mínima de um retângulo [minX..maxX] x [minY..maxY] (inclusive,
    // em unidades da grade do NÍVEL DADO) por células de nível <= maxLevel:
    // células totalmente contidas são emitidas no nível mais profundo
    // possível; células de borda no nível maxLevel. Determinístico (ordem de
    // varredura fixa). Recusa retângulo inválido ou cap excedido.
    virtual bool cover(int minX, int minY, int maxX, int maxY, int level,
                       std::vector<std::uint64_t>& out,
                       std::string& errorOut) const = 0;

    // True se a célula (x, y, level) é exatamente `cellId`.
    virtual bool contains(std::uint64_t cellId, int x, int y,
                          std::string& errorOut) const = 0;
};

// Fábrica do adapter (o único TU que implementa IHilbertCellIndex).
std::unique_ptr<IHilbertCellIndex> create_hilbert_cell_index(
    std::string& errorOut);
std::unique_ptr<IHilbertCellIndex> create_hilbert_cell_index_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace world
}  // namespace engine
