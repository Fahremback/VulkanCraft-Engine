#include <engine/entity/IReflection.hpp>
#include <engine/assets/IAssetFormats.hpp>
#include <engine/networking/ITransport.hpp>
#include <engine/plugins/IPluginPermissions.hpp>
#include <engine/procgen/IJobService.hpp>
#include <engine/scripting/IVisualScriptGraph.hpp>
#include <engine/scripting/IVisualScriptRuntime.hpp>
#include <cassert>
#include <iostream>
int main(){std::string e;auto reflection=engine::entity::create_reflection();assert(reflection);engine::entity::TypeInfo externalType{"ExternalType","external.type","","1.0.0",{{"id",engine::entity::FieldKind::Uuid}}};assert(reflection->register_type(externalType,e));auto formats=engine::assets::create_asset_format_registry();assert(formats);assert(formats->import_document({engine::assets::AssetFormat::Text,"external.txt",{'o','k'},{}},e));auto transport=engine::networking::create_transport({engine::networking::TransportKind::Loopback,{"127.0.0.1",9000},1024,8,false},e);assert(transport);assert(transport->start({engine::networking::TransportKind::Loopback,{"127.0.0.1",9000},1024,8,false},e));assert(transport->connect({"127.0.0.1",9000},e));assert(transport->send({'o','k'},e));assert(transport->poll(e).size()==1);auto policy=engine::plugins::create_plugin_permission_policy();assert(policy&&policy->grant("assets:read","external"));assert(policy->is_granted("assets:read","external"));auto jobs=engine::jobs::create_job_service();auto id=jobs->start("external",1000,e);assert(id&&jobs->update(id,.5,"running",e));assert(jobs->complete(id,{},e));engine::scripting::VisualScriptGraph graph;assert(graph.register_node_type({"event","Event","",{},{}},&e));auto node=graph.add_node({0,"event",{}, {}, {}});auto runtime=engine::scripting::create_visual_script_runtime();assert(runtime&&runtime->load(graph,e));assert(runtime->emit_event("start",{},e));assert(runtime->step(1,e));std::cout<<"sdk-surface-consumer-ok\n";}
