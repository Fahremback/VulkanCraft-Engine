#pragma once
#include "engine/scripting/IVisualScriptGraph.hpp"
#include "engine/scripting/IVisualScriptRuntime.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::scripting {
struct VisualScriptServiceDescriptor { std::string stable_id; std::string version{"1.0.0"}; std::vector<std::string> events; };
class IVisualScriptService {
public:
    virtual ~IVisualScriptService() = default;
    virtual bool register_service(const VisualScriptServiceDescriptor&, std::string&) = 0;
    virtual bool attach(const std::string&, IVisualScriptRuntime&, std::string&) = 0;
    virtual bool dispatch(const std::string&, const std::string&, const std::vector<PinValue>&, std::string&) = 0;
    virtual std::vector<VisualScriptServiceDescriptor> services() const = 0;
    virtual void clear() = 0;
};
std::unique_ptr<IVisualScriptService> create_visual_script_service();
}
