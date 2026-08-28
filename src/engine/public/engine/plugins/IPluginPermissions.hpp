#pragma once
#include "engine/plugins/IPluginSandbox.hpp"
#include <memory>
#include <string>
#include <vector>
namespace engine::plugins {
struct PermissionDecision { std::string permission; bool allowed{false}; std::string reason; };
class IPluginPermissionPolicy { public: virtual ~IPluginPermissionPolicy()=default; virtual PermissionDecision evaluate(const std::string&,const std::string&)const=0; virtual std::vector<PermissionDecision> evaluate_all(const std::string&,const std::vector<std::string>&)const=0; virtual bool grant(const std::string&,const std::string&)=0; virtual bool revoke(const std::string&,const std::string&)=0; virtual bool is_granted(const std::string&,const std::string&)const=0; };
std::unique_ptr<IPluginPermissionPolicy> create_plugin_permission_policy();
}
