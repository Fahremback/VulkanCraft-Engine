#include "engine/scripting/IVisualScriptService.hpp"
#include <cassert>
#include <iostream>
int main(){auto s=engine::scripting::create_visual_script_service();auto r=engine::scripting::create_visual_script_runtime();std::string e;assert(s->register_service({"game.service","1.0.0",{"start"}},e));assert(s->attach("game.service",*r,e));assert(!s->dispatch("game.service","start",{},e));std::cout<<"visual-script-service-ok\n";}
