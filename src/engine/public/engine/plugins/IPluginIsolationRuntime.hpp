#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace engine::plugins {
class IPluginIsolationRuntime {
public:
    virtual ~IPluginIsolationRuntime() = default;
    virtual bool register_plugin(const std::string&, std::uint64_t, std::uint64_t, std::string&) = 0;
    virtual bool begin_call(const std::string&, std::string&) = 0;
    virtual bool end_call(const std::string&, std::uint64_t, std::uint64_t, std::string&) = 0;
    virtual bool cancel(const std::string&, std::string&) = 0;
    virtual bool healthy(const std::string&) const = 0;
    virtual bool unload(const std::string&, std::string&) = 0;
};
std::unique_ptr<IPluginIsolationRuntime> create_plugin_isolation_runtime();
}
