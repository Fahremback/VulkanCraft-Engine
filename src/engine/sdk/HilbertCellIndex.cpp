// HilbertCellIndex.cpp — the only TU implementing IHilbertCellIndex.
//
// Indexação espacial hierárquica estilo S2 (sem face) implementada do zero:
// células de uma quadtree ordenadas pela curva de Hilbert. Codificação:
//   cellId = (1 << (2*level)) | (hilbertD << 1) | 1
//   level = posição do MSB / 2; hilbertD em [0, 2^(2*level)).
//
// Determinismo: xy->d e d->xy são os algoritmos clássicos da curva de
// Hilbert (rotações por subgrade) sem RNG; o cover é uma descida de
// quadtree com ordem de quadrante fixa. Sem threading.

#include "engine/world/IHilbertCellIndex.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace engine {
namespace world {
namespace {

bool HilbertCellConfig_validate(const HilbertCellConfig& config,
                                std::string& errorOut) {
    if (config.maxLevel < 1 || config.maxLevel > 30) {
        errorOut = "hilbert config: maxLevel must be in [1, 30]";
        return false;
    }
    if (config.maxCoverCells < 1 || config.maxCoverCells > (1ull << 20)) {
        errorOut = "hilbert config: maxCoverCells must be in [1, 1<<20]";
        return false;
    }
    return true;
}

// xy -> d na grade 2^level x 2^level (curva de Hilbert, rotações padrão).
inline std::uint64_t hilbert_xy_to_d(std::uint64_t x, std::uint64_t y,
                                     int level) {
    if (level <= 0) return 0;  // domínio inteiro é uma célula única
    std::uint64_t d = 0;
    std::uint64_t s = 1ull << (level - 1);
    while (s > 0) {
        const std::uint64_t rx = (x & s) ? 1 : 0;
        const std::uint64_t ry = (y & s) ? 1 : 0;
        d += s * s * ((3 * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) {
                x = s - 1 - x;
                y = s - 1 - y;
            }
            const std::uint64_t t = x;
            x = y;
            y = t;
        }
        s >>= 1;
    }
    return d;
}

// d -> xy (inversa da acima).
inline void hilbert_d_to_xy(std::uint64_t d, int level, std::uint64_t& x,
                            std::uint64_t& y) {
    const std::uint64_t n = 1ull << level;
    x = 0;
    y = 0;
    std::uint64_t t = d;
    for (std::uint64_t s = 1; s < n; s <<= 1) {
        const std::uint64_t rx = 1 & (t / 2);
        const std::uint64_t ry = 1 & (t ^ rx);
        if (ry == 0) {
            if (rx == 1) {
                x = s - 1 - x;
                y = s - 1 - y;
            }
            const std::uint64_t swp = x;
            x = y;
            y = swp;
        }
        x += s * rx;
        y += s * ry;
        t /= 4;
    }
}

// Codifica (level, hilbertD) no cell id canônico. O marcador de nível fica no
// bit 2*level+1 (acima do maior índice possível, d < 2^(2*level)), então não
// há colisão entre marcador e índice (d<<1 ocupa bits [1, 2*level]).
inline std::uint64_t encode(int level, std::uint64_t d) {
    return (1ull << (2 * level + 1)) | ((d << 1) | 1ull);
}

// Nível do cell id: (posição do MSB - 1) / 2.
inline int decode_level(std::uint64_t cellId) {
    int msb = 0;
    std::uint64_t v = cellId;
    while (v > 1) {
        v >>= 1;
        ++msb;
    }
    return (msb - 1) / 2;
}

// hilbertD do cell id.
inline std::uint64_t decode_d(std::uint64_t cellId, int level) {
    const std::uint64_t mask = (1ull << (2 * level)) - 1;
    return (cellId >> 1) & mask;
}

class HilbertCellIndex final : public IHilbertCellIndex {
public:
    const HilbertCellConfig& config() const noexcept override {
        return config_;
    }

    bool configure(const HilbertCellConfig& config,
                   std::string& errorOut) override {
        if (!HilbertCellConfig_validate(config, errorOut)) return false;
        config_ = config;
        return true;
    }

    std::uint64_t cell_id(int x, int y, int level,
                          std::string& errorOut) const override {
        if (level < 0 || level > static_cast<int>(config_.maxLevel)) {
            errorOut = "hilbert: level out of [0, maxLevel]";
            return 0;
        }
        const int n = 1 << level;
        if (x < 0 || y < 0 || x >= n || y >= n) {
            errorOut = "hilbert: (x, y) out of the 2^level grid";
            return 0;
        }
        return encode(level, hilbert_xy_to_d(
                                  static_cast<std::uint64_t>(x),
                                  static_cast<std::uint64_t>(y), level));
    }

    bool cell_position(std::uint64_t cellId, int& x, int& y,
                       std::string& errorOut) const override {
        if (cellId == 0) {
            errorOut = "hilbert: invalid cell id 0";
            return false;
        }
        const int level = decode_level(cellId);
        if (level > static_cast<int>(config_.maxLevel)) {
            errorOut = "hilbert: cell id level above maxLevel";
            return false;
        }
        std::uint64_t ux = 0;
        std::uint64_t uy = 0;
        hilbert_d_to_xy(decode_d(cellId, level), level, ux, uy);
        x = static_cast<int>(ux);
        y = static_cast<int>(uy);
        return true;
    }

    int cell_level(std::uint64_t cellId) const noexcept override {
        return cellId == 0 ? 0 : decode_level(cellId);
    }

    std::uint64_t parent_cell(std::uint64_t cellId) const noexcept override {
        if (cellId == 0) return 0;
        const int level = decode_level(cellId);
        if (level == 0) return cellId;  // raiz é pai dela mesma
        const std::uint64_t d = decode_d(cellId, level);
        return encode(level - 1, d >> 2);
    }

    std::vector<std::uint64_t> children_cells(
        std::uint64_t cellId) const noexcept override {
        std::vector<std::uint64_t> out;
        if (cellId == 0) return out;
        const int level = decode_level(cellId);
        if (level >= static_cast<int>(config_.maxLevel)) return out;
        const std::uint64_t d = decode_d(cellId, level);
        for (std::uint64_t k = 0; k < 4; ++k) {
            out.push_back(encode(level + 1, (d << 2) | k));
        }
        return out;
    }

    bool cover(int minX, int minY, int maxX, int maxY, int level,
               std::vector<std::uint64_t>& out,
               std::string& errorOut) const override {
        out.clear();
        if (level < 0 || level > static_cast<int>(config_.maxLevel)) {
            errorOut = "hilbert: level out of [0, maxLevel]";
            return false;
        }
        const int n = 1 << level;
        if (minX < 0 || minY < 0 || maxX >= n || maxY >= n ||
            maxX < minX || maxY < minY) {
            errorOut = "hilbert: rect must be inside [0, 2^level) and ordered";
            return false;
        }
        std::vector<std::uint64_t> cells;
        // Descida de quadtree a partir da raiz (ordem de quadrante fixa:
        // NW, NE, SW, SE — determinística).
        cover_rec(cells, 0, 0, 0, 1 << level, minX, minY, maxX, maxY, level,
                  errorOut);
        if (!errorOut.empty()) {
            out.clear();
            return false;
        }
        out = std::move(cells);
        return true;
    }

    bool contains(std::uint64_t cellId, int x, int y,
                  std::string& errorOut) const override {
        if (cellId == 0) {
            errorOut = "hilbert: invalid cell id 0";
            return false;
        }
        const int level = decode_level(cellId);
        if (level > static_cast<int>(config_.maxLevel)) {
            errorOut = "hilbert: cell id level above maxLevel";
            return false;
        }
        const int n = 1 << level;
        if (x < 0 || y < 0 || x >= n || y >= n) {
            errorOut = "hilbert: (x, y) out of the 2^level grid";
            return false;
        }
        return cell_id(x, y, level, errorOut) == cellId;
    }

private:
    // Descida de quadtree em unidades da grade do nível alvo: o domínio é
    // 2^level de lado; uma célula no nível L tem lado 2^(level-L) e canto
    // (x0, y0). COBERTURA MÍNIMA: uma célula totalmente contida é emitida no
    // nível mais profundo possível (o dela); uma célula de borda é emitida
    // no nível alvo. Ordem de quadrante fixa (NW, NE, SW, SE).
    void cover_rec(std::vector<std::uint64_t>& cells, int x0, int y0,
                   int cellLevel, int side, int minX, int minY, int maxX,
                   int maxY, int targetLevel, std::string& errorOut) const {
        if (cells.size() >= config_.maxCoverCells) {
            errorOut = "hilbert: cover exceeds maxCoverCells";
            return;
        }
        const int x1 = x0 + side - 1;
        const int y1 = y0 + side - 1;
        if (x1 < minX || y1 < minY || x0 > maxX || y0 > maxY) return;  // fora
        const bool inside = x0 >= minX && y0 >= minY && x1 <= maxX && y1 <= maxY;
        if (inside) {
            cells.push_back(cell_id(x0 >> (targetLevel - cellLevel),
                                    y0 >> (targetLevel - cellLevel),
                                    cellLevel, errorOut));
            return;
        }
        if (cellLevel == targetLevel) {
            cells.push_back(cell_id(x0, y0, cellLevel, errorOut));  // borda
            return;
        }
        const int half = side / 2;
        cover_rec(cells, x0, y0, cellLevel + 1, half, minX, minY, maxX, maxY,
                  targetLevel, errorOut);                        // NW
        cover_rec(cells, x0 + half, y0, cellLevel + 1, half, minX, minY, maxX,
                  maxY, targetLevel, errorOut);                  // NE
        cover_rec(cells, x0, y0 + half, cellLevel + 1, half, minX, minY, maxX,
                  maxY, targetLevel, errorOut);                  // SW
        cover_rec(cells, x0 + half, y0 + half, cellLevel + 1, half, minX, minY,
                  maxX, maxY, targetLevel, errorOut);            // SE
    }

    HilbertCellConfig config_;
};

}  // namespace

bool HilbertCellConfig::valid(std::string& errorOut) const {
    return HilbertCellConfig_validate(*this, errorOut);
}

bool HilbertCellConfig::load_from_json(const std::string& json,
                                       std::string& errorOut) {
    HilbertCellConfig candidate = *this;
    bool any = false;
    std::size_t pos = 0;
    while (pos < json.size()) {
        const std::size_t kStart = json.find('"', pos);
        if (kStart == std::string::npos) break;
        const std::size_t kEnd = json.find('"', kStart + 1);
        if (kEnd == std::string::npos) break;
        const std::string key = json.substr(kStart + 1, kEnd - kStart - 1);
        const std::size_t colon = json.find(':', kEnd);
        if (colon == std::string::npos) break;
        const std::size_t vStart = json.find_first_not_of(" \t\r\n", colon + 1);
        if (vStart == std::string::npos) break;
        const std::size_t vEnd = json.find_first_of(",}", vStart);
        const std::string value =
            json.substr(vStart, vEnd == std::string::npos ? std::string::npos
                                                          : vEnd - vStart);
        if (key == "maxLevel") {
            candidate.maxLevel = static_cast<std::uint32_t>(
                std::strtoul(value.c_str(), nullptr, 10));
            any = true;
        } else if (key == "maxCoverCells") {
            candidate.maxCoverCells = static_cast<std::uint64_t>(
                std::strtoull(value.c_str(), nullptr, 10));
            any = true;
        }
        pos = vEnd == std::string::npos ? json.size() : vEnd + 1;
    }
    if (!any) {
        errorOut = "hilbert config: no recognized keys";
        return false;
    }
    if (!HilbertCellConfig_validate(candidate, errorOut)) return false;
    *this = candidate;
    return true;
}

std::string HilbertCellConfig::to_json() const {
    std::string out = "{";
    out += "\"maxLevel\":" + std::to_string(maxLevel) + ",";
    out += "\"maxCoverCells\":" + std::to_string(maxCoverCells);
    out += "}";
    return out;
}

std::unique_ptr<IHilbertCellIndex> create_hilbert_cell_index(
    std::string& errorOut) {
    auto impl = std::make_unique<HilbertCellIndex>();
    if (!impl) {
        errorOut = "hilbert: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IHilbertCellIndex> create_hilbert_cell_index_json(
    const std::string& jsonText, std::string& errorOut) {
    HilbertCellConfig config;
    if (!config.load_from_json(jsonText, errorOut)) return nullptr;
    auto impl = std::make_unique<HilbertCellIndex>();
    if (!impl->configure(config, errorOut)) return nullptr;
    return impl;
}

}  // namespace world
}  // namespace engine
