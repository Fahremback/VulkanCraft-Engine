#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::capabilities {

enum class CapabilityKind : std::uint8_t { Type, Component, Asset, Command, Event, Service };

struct CapabilityDescriptor {
    std::string stable_id;
    std::string display_name;
    CapabilityKind kind{CapabilityKind::Service};
    std::string version{"1.0.0"};
    std::string alias;
    bool deprecated{false};
};

class ICapabilityRegistry {
public:
    virtual ~ICapabilityRegistry() = default;
    virtual bool register_capability(const CapabilityDescriptor&, std::string&) = 0;
    virtual bool unregister_capability(const std::string&, std::string&) = 0;
    virtual const CapabilityDescriptor* find(const std::string&) const = 0;
    virtual std::vector<CapabilityDescriptor> list() const = 0;
    virtual std::vector<CapabilityDescriptor> list(CapabilityKind) const = 0;
    virtual std::string to_json() const = 0;
    virtual bool load_json(const std::string&, std::string&) = 0;
    virtual void clear() = 0;
};

std::unique_ptr<ICapabilityRegistry> create_capability_registry();

} // namespace engine::capabilities
