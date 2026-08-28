#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace engine::networking {
enum class TransportKind:std::uint8_t{Loopback,Udp,Tcp};enum class TransportState:std::uint8_t{Stopped,Listening,Connected,Failed};
struct TransportEndpoint{std::string host;std::uint16_t port{0};};
struct TransportMessage{std::uint64_t sequence{0};std::uint64_t peer_id{0};bool reliable{true};std::vector<std::uint8_t> payload;};
struct TransportConfig{TransportKind kind{TransportKind::Loopback};TransportEndpoint endpoint;std::uint32_t max_message_bytes{1024*1024};std::uint32_t poll_budget{64};bool reuse_address{false};};
class ITransport{public:virtual~ITransport()=default;virtual bool start(const TransportConfig&,std::string&)=0;virtual bool connect(const TransportEndpoint&,std::string&)=0;virtual bool send(const std::vector<std::uint8_t>&,std::string&)=0;virtual std::vector<TransportMessage>poll(std::string&)=0;virtual bool cancel(std::string&)=0;virtual void stop()noexcept=0;virtual TransportState state()const noexcept=0;virtual TransportEndpoint local_endpoint()const=0;virtual TransportEndpoint remote_endpoint()const=0;virtual std::uint64_t sent_count()const noexcept=0;virtual std::uint64_t received_count()const noexcept=0;virtual bool set_peer_id(std::uint64_t,std::string&)=0;virtual std::uint64_t peer_id()const noexcept=0;};
std::unique_ptr<ITransport>create_transport(const TransportConfig&,std::string&);
}
