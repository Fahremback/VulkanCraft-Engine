#include "NetworkingPlugin.hpp"
#include "engine/networking/INetworkServer.hpp"
#include <cstdint>
namespace Engine::Plugins {

// helper exposto ao editor/MCP: monta a DedicatedServerConfig a partir de
// valores simples (como o MCP/editor configura uma sessão). DELEGA à factory
// pública do SDK (engine::networking::make_dedicated_server_config, Agente 3
// §I) — a MESMA consumida pelo servidor dedicado real (main_server.cpp), de
// modo que editor, MCP e servidor compartilham uma única fonte de verdade e a
// negociação de versão canônica (todos os eixos em 1).
engine::networking::DedicatedServerConfig make_dedicated_server_config(
    const std::string& serverId, std::uint16_t port,
    std::uint32_t tickRate, std::uint32_t maxClients, bool udp) {
    return engine::networking::make_dedicated_server_config(
        serverId, port, tickRate, maxClients, udp);
}

void NetworkingPlugin::register_types(TypeRegistry&r){
    register_plugin_type(r,"NetworkIdentityComponent");
    register_plugin_type(r,"ReplicatedComponent");
    // Componentes canônicos da pilha (seções D/F/G): ownership replica da
    // sessão, autoridade local de comandos, e predição no inspector.
    register_plugin_type(r,"NetworkSessionComponent");
    register_plugin_type(r,"NetworkAuthorityComponent");
    register_plugin_type(r,"PredictionController");
    register_plugin_type(r,"ReplicatedTransform");
}
void NetworkingPlugin::register_assets(AssetRegistry&r){(void)r;}
void NetworkingPlugin::register_editor_tools(Editor::EditorRegistry&r){
    register_asset_tool(r,"ReplicationProfileAsset","Tools/Networking",get_name());
    register_viewport_tool(r,"Network Relevancy Tool","Tools/Networking",get_name());
    // Exposição de configuração de sessão/multiplayer no editor (seção I).
    register_asset_tool(r,"DedicatedServerConfig","Tools/Networking",get_name());
}}
