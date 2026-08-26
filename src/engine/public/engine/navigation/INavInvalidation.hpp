// INavInvalidation — ledger de invalidação LOCALIZADA da navegação.
// Componente CORE do §2 item 24 ("atualização localizada da navegação após
// adicionar/remover blocos, portas e obstáculos dinâmicos"): quando o mundo
// muda (bloco adicionado/removido, porta, obstáculo dinâmico), o chamador
// marca a REGIÃO afetada; o ledger calcula quais tiles de navmesh intersectam
// e os marca como inválidos com uma VERSÃO de invalidação. O runtime de
// navmesh reconstrói só esses tiles (nunca o mapa inteiro) e `rebuild` limpa.
// `invalidated_since(version)` serve a retomada após streaming/carregamento
// (item 29): quem voltou a carregar uma região sabe quais tiles refazer.
// Puro, determinístico (tiles sempre em ordem (x,z)); sem navmesh de fato.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace navigation {

struct NavTile {
    std::int32_t x{ 0 };
    std::int32_t z{ 0 };
    bool operator<(const NavTile& other) const {
        if (x != other.x) return x < other.x;
        return z < other.z;
    }
    bool operator==(const NavTile& other) const {
        return x == other.x && z == other.z;
    }
};

struct NavInvalidationRegion {
    float minX{ 0.0f };
    float minZ{ 0.0f };
    float maxX{ 0.0f };
    float maxZ{ 0.0f };
};

class INavInvalidation {
public:
    virtual ~INavInvalidation() = default;

    // All-or-nothing: tileSize <= 0 ou não-finito recusa.
    virtual bool configure(float tileSize, std::string& errorOut) = 0;

    // Tiles que intersectam a região, em ordem (x,z) — sem mutar nada.
    virtual std::vector<NavTile> tiles_for(
        const NavInvalidationRegion& region) const = 0;

    // Marca os tiles intersectantes como inválidos e incrementa a versão.
    // Região inválida (min > max, não-finita) é no-op.
    virtual void invalidate(const NavInvalidationRegion& region) = 0;

    virtual bool is_invalid(const NavTile& tile) const = 0;

    // Reconstrói o tile (limpa a marca de inválido).
    virtual bool rebuild(const NavTile& tile) = 0;

    // Tiles atualmente inválidos (ordem (x,z)).
    virtual std::vector<NavTile> invalid_tiles() const = 0;

    // Nº de invalidações já aplicadas.
    virtual std::uint64_t version() const = 0;

    // Tiles marcados como inválidos por invalidações com versão > `version`
    // e ainda NÃO reconstruídos — a retomada (item 29) chama com a versão
    // que já processou. Ordem (x,z).
    virtual std::vector<NavTile> invalidated_since(std::uint64_t version) const = 0;

    virtual void clear() = 0;
};

std::unique_ptr<INavInvalidation> create_nav_invalidation();

}  // namespace navigation
}  // namespace engine
