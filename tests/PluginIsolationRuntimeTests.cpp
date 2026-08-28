#include "engine/plugins/IPluginIsolationRuntime.hpp"
#include <cassert>
#include <iostream>
int main(){auto r=engine::plugins::create_plugin_isolation_runtime();std::string e;assert(r->register_plugin("p",10,100,e));assert(r->begin_call("p",e));assert(!r->begin_call("p",e));assert(!r->end_call("p",11,1,e));assert(!r->healthy("p"));assert(r->unload("p",e));std::cout<<"plugin-isolation-runtime-ok\n";}
