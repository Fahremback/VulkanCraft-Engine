#pragma once
#include "../PluginContract.hpp"
#include <span>
namespace Engine::Plugins {
using ConnectionId=uint64_t;enum class NetworkReliability:uint8_t{Unreliable,Reliable,Ordered};struct NetworkPacket{ConnectionId peer{};uint16_t channel{};NetworkReliability reliability{};std::vector<std::byte>payload;};
class INetworkTransport{public:virtual~INetworkTransport()=default;virtual bool listen(uint16_t port)=0;virtual ConnectionId connect(std::string_view host,uint16_t port)=0;virtual bool send(const NetworkPacket&packet)=0;virtual std::vector<NetworkPacket>poll()=0;virtual void disconnect(ConnectionId peer)=0;};
class NetworkingPlugin final:public EnginePlugin{public:explicit NetworkingPlugin(std::shared_ptr<INetworkTransport>s={}):transport_(std::move(s)){}std::string get_name()const override{return "Networking";}std::string get_version()const override{return "1.0.0";}INetworkTransport*transport()const noexcept{return transport_.get();}protected:void register_types(TypeRegistry&)override;void register_assets(AssetRegistry&)override;void register_editor_tools(Editor::EditorRegistry&)override;private:std::shared_ptr<INetworkTransport>transport_;};}
