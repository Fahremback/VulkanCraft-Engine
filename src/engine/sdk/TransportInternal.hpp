#pragma once
// Internal (sdk-only) transport backend factories. The canonical public
// `create_transport` (Transport.cpp) dispatches to these so the deterministic
// Loopback backend and the real UDP backend live in separate translation units.
#include "engine/networking/ITransport.hpp"
#include <memory>
#include <string>
namespace engine {
namespace networking {
std::unique_ptr<ITransport> create_loopback_transport(const TransportConfig&);
std::unique_ptr<ITransport> create_udp_transport(const TransportConfig&, std::string&);
// Client-side UDP transport used by the dedicated server per accepted client.
std::unique_ptr<ITransport> create_udp_client_transport(const TransportConfig&, std::string&);
}  // namespace networking
}  // namespace engine