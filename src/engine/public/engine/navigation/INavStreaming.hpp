// INavStreaming — gate de REGIÕES ATIVAS para o streaming de navegação.
// Componente CORE do §2 item 29 ("integrar navegação ao streaming: somente
// regiões ativas, invalidação segura e retomada após carregamento"): o foco
// (tile do jogador) define um quadrado de raio R de tiles ativos; o ledger
// responde quais tiles carregar/descarregar conforme o foco move, e quais
// tiles ativos estão inválidos aguardando rebuild (retomada segura — o tile
// só volta a valer depois de rebuild). Puro e determinístico (listas sempre
// em ordem (x,z)); o runtime de navmesh executa o carregamento de fato.

#pragma once

#include "engine/navigation/INavInvalidation.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace navigation {

class INavStreaming {
public:
    virtual ~INavStreaming() = default;

    // All-or-nothing: radiusTiles == 0 recusa. Substitui o estado.
    virtual bool configure(std::size_t radiusTiles, std::string& errorOut) = 0;

    // Move o foco (tile central da região ativa).
    virtual void set_focus(std::int32_t tileX, std::int32_t tileZ) = 0;
    virtual void focus(NavTile& out) const = 0;

    // Tile dentro do quadrado foco ± raio.
    virtual bool is_tile_active(const NavTile& tile) const = 0;

    // Ativos ainda não carregados → carregar agora (ordem (x,z)).
    virtual std::vector<NavTile> tiles_to_load() const = 0;

    // Carregados e fora do raio → descarregar (ordem (x,z)).
    virtual std::vector<NavTile> tiles_to_unload() const = 0;

    virtual bool is_loaded(const NavTile& tile) const = 0;
    virtual bool mark_loaded(const NavTile& tile) = 0;    // false se não-ativo
    virtual bool mark_unloaded(const NavTile& tile) = 0;  // false se não-carregado

    // Marca um tile ativo como inválido (mundo mudou) — retomada segura:
    // ele continua carregado mas só volta a valer após rebuild.
    virtual bool invalidate_tile(const NavTile& tile) = 0;

    // Tiles ativos E inválidos aguardando rebuild (ordem (x,z)).
    virtual std::vector<NavTile> tiles_pending_rebuild() const = 0;

    virtual std::size_t loaded_count() const = 0;
    virtual void clear() = 0;
};

std::unique_ptr<INavStreaming> create_nav_streaming();

}  // namespace navigation
}  // namespace engine
