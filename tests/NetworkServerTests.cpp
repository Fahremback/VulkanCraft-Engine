#include "engine/networking/INetworkServer.hpp"
#include <cassert>
int main() {
    using namespace engine::networking;
    auto server = create_network_server();
    DedicatedServerConfig config;
    config.server_id = "test";
    config.transport.kind = TransportKind::Loopback;
    config.transport.endpoint = {"127.0.0.1", 4001};
    config.max_clients = 2;
    std::string error;
    assert(server->start(config, error));
    assert(server->accept_client(1, error));
    assert(server->accept_client(2, error));
    assert(!server->accept_client(3, error));
    assert(server->tick(error));
    assert(server->metrics().tick == 1);
    assert(server->metrics().clients == 2);
    assert(server->remove_client(1, error));
    assert(server->stop(error));
    assert(server->state() == TransportState::Stopped);
    return 0;
}
