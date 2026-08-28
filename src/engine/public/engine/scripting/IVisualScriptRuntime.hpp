#pragma once
#include "engine/scripting/IVisualScriptGraph.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace engine::scripting {
struct ScriptError { std::string code; std::string message; std::uint64_t node_id{0}; };
struct ScriptProfile { std::uint64_t steps{0}; std::uint64_t elapsed_ticks{0}; };
struct ScriptBreakpoint { std::uint64_t node_id{0}; bool enabled{true}; };
class IVisualScriptRuntime {
public:
 virtual ~IVisualScriptRuntime() = default;
 virtual bool load(const IVisualScriptGraph&, std::string&) = 0;
 virtual bool set_variable(const std::string&, const PinValue&, std::string&) = 0;
 virtual const PinValue* variable(const std::string&) const = 0;
 virtual bool emit_event(const std::string&, const std::vector<PinValue>&, std::string&) = 0;
 virtual bool step(std::uint32_t, std::string&) = 0;
 virtual bool run_until_yield(std::string&) = 0;
 virtual bool resume(std::string&) = 0;
 virtual bool add_breakpoint(const ScriptBreakpoint&, std::string&) = 0;
 virtual bool remove_breakpoint(std::uint64_t) = 0;
 virtual std::vector<ScriptBreakpoint> breakpoints() const = 0;
 virtual std::vector<std::string> variables() const = 0;
 virtual const ScriptError* last_error() const = 0;
 virtual ScriptProfile profile() const = 0;
 virtual bool paused() const noexcept = 0;
};
std::unique_ptr<IVisualScriptRuntime> create_visual_script_runtime();
}
