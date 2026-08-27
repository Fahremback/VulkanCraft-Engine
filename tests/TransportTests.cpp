#include "engine/networking/ITransport.hpp"
#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    using namespace engine::networking;
    std::string error;
    TransportConfig config;
    config.kind = TransportKind::Loopback;
    config.endpoint = {"127.0.0.1", 4000};
    config.max_message_bytes = 4;
    config.poll_budget = 1;
    auto transport = create_transport(config, error);
    assert(transport && error.empty());
    assert(!transport->send({1}, error));
    assert(transport->start(config, error));
    assert(transport->connect(config.endpoint, error));
    assert(!transport->send({1,2,3,4,5}, error));
    assert(transport->send({1,2}, error));
    assert(transport->send({3}, error));
    auto first = transport->poll(error);
    assert(first.size() == 1 && first[0].sequence == 0 && first[0].payload[1] == 2);
    auto second = transport->poll(error);
    assert(second.size() == 1 && second[0].sequence == 1);
    assert(transport->sent_count() == 2 && transport->received_count() == 2);
    assert(transport->send({4}, error));
    assert(transport->cancel(error));
    assert(transport->poll(error).empty());
    transport->stop();
    assert(transport->state() == TransportState::Stopped);
    TransportConfig unsupported = config;
    unsupported.kind = TransportKind::Udp;
    auto none = create_transport(unsupported, error);
    assert(!none && error == "transport_backend_unavailable");
    return 0;
}
