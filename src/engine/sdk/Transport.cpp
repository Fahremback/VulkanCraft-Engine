#include "engine/networking/ITransport.hpp"
#include <algorithm>
#include <deque>
#include <utility>
namespace engine::networking { namespace {
class Loopback final:public ITransport{
 TransportConfig cfg_;TransportEndpoint local_,remote_;TransportState state_{TransportState::Stopped};std::deque<TransportMessage> q_;std::uint64_t next_{0},sent_{0},received_{0},peer_{0};
public:
 explicit Loopback(TransportConfig c):cfg_(std::move(c)),local_(cfg_.endpoint){}
 bool start(const TransportConfig&c,std::string&e)override{if(c.max_message_bytes==0||c.poll_budget==0){e="invalid_transport_config";return false;}cfg_=c;local_=c.endpoint;q_.clear();next_=0;sent_=received_=0;state_=TransportState::Listening;return true;}
 bool connect(const TransportEndpoint&e,std::string&err)override{if(state_!=TransportState::Listening&&state_!=TransportState::Connected){err="transport_not_started";return false;}if(e.port==0){err="invalid_endpoint";return false;}remote_=e;state_=TransportState::Connected;return true;}
 bool send(const std::vector<std::uint8_t>&p,std::string&e)override{if(state_!=TransportState::Connected){e="transport_not_connected";return false;}if(p.empty()||p.size()>cfg_.max_message_bytes){e="invalid_payload";return false;}q_.push_back({next_++,peer_,true,p});++sent_;return true;}
 std::vector<TransportMessage>poll(std::string&)override{std::vector<TransportMessage>o;auto n=std::min<std::size_t>(cfg_.poll_budget,q_.size());while(n--){o.push_back(std::move(q_.front()));q_.pop_front();++received_;}return o;}
 bool cancel(std::string&)override{q_.clear();return true;}void stop()noexcept override{q_.clear();state_=TransportState::Stopped;}TransportState state()const noexcept override{return state_;}TransportEndpoint local_endpoint()const override{return local_;}TransportEndpoint remote_endpoint()const override{return remote_;}std::uint64_t sent_count()const noexcept override{return sent_;}std::uint64_t received_count()const noexcept override{return received_;}bool set_peer_id(std::uint64_t id,std::string&e)override{if(id==0){e="invalid_peer_id";return false;}peer_=id;return true;}std::uint64_t peer_id()const noexcept override{return peer_;}
};}
std::unique_ptr<ITransport>create_transport(const TransportConfig&c,std::string&e){if(c.endpoint.host.empty()||c.endpoint.port==0){e="invalid_endpoint";return{};}if(c.kind!=TransportKind::Loopback){e="transport_backend_unavailable";return{};}if(c.max_message_bytes==0||c.poll_budget==0){e="invalid_transport_config";return{};}return std::make_unique<Loopback>(c);}
}
