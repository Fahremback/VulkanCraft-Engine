#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace engine::networking {
enum class TransportKind:std::uint8_t{Loopback,Udp,Tcp};enum class TransportState:std::uint8_t{Stopped,Listening,Connected,Failed};
struct TransportEndpoint{std::string host;std::uint16_t port{0};};
struct TransportMessage{std::uint64_t sequence{0};std::uint64_t peer_id{0};bool reliable{true};std::vector<std::uint8_t> payload;
    // Channel mapping (canonical three-use stack, sections B.3 of agente3):
    //   0 = reliable ordered (default; commands, edits, critical state)
    //   1 = reliable unordered (latest wins; transform/state overwrites)
    //   2 = unreliable sequenced (snapshots / frequent updates)
    std::uint16_t channel{0};
    bool unordered{false};
    // Received wall-clock lag (ms) carried for jitter/backpressure tuning (0 = unset).
    std::uint32_t rtt_millis{0};};
struct TransportConfig{TransportKind kind{TransportKind::Loopback};TransportEndpoint endpoint;std::uint32_t max_message_bytes{1024*1024};std::uint32_t poll_budget{64};bool reuse_address{false};};
class ITransport{public:virtual~ITransport()=default;virtual bool start(const TransportConfig&,std::string&)=0;virtual bool connect(const TransportEndpoint&,std::string&)=0;virtual bool send(const std::vector<std::uint8_t>&,std::string&)=0;
// Channel-aware send (canonical stack, section B.3): reliable ordered (0),
// reliable unordered/latest-wins (1), unreliable sequenced (2).
virtual bool send_channel(std::uint16_t channel,bool reliable,bool unordered,const std::vector<std::uint8_t>&,std::string&)=0;
// Shorthand for channel 2 (frequent snapshots, drop-on-loss, no retransmit).
virtual bool send_unreliable(const std::vector<std::uint8_t>&,std::string&)=0;
virtual std::vector<TransportMessage>poll(std::string&)=0;virtual bool cancel(std::string&)=0;virtual void stop()noexcept=0;virtual TransportState state()const noexcept=0;virtual TransportEndpoint local_endpoint()const=0;virtual TransportEndpoint remote_endpoint()const=0;virtual std::uint64_t sent_count()const noexcept=0;virtual std::uint64_t received_count()const noexcept=0;virtual bool set_peer_id(std::uint64_t,std::string&)=0;virtual std::uint64_t peer_id()const noexcept=0;};
std::unique_ptr<ITransport>create_transport(const TransportConfig&,std::string&);
}
