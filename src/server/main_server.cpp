#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include "../engine/scene/Scene.hpp"
#include "../engine/scene/Entity.hpp"
#include "../engine/networking/NetworkRuntime.hpp"

int main(int argc,char**argv){
 int tickCount=10;
 for(int i=1;i<argc;++i)if(std::string_view(argv[i])=="--ticks"&&i+1<argc)tickCount=std::max(0,std::atoi(argv[++i]));
 Engine::Scene scene("Headless Dedicated Server Scene");
 auto player=scene.create_entity("Server Player");auto npc=scene.create_entity("Server NPC");
 scene.set_parent(npc.get_id(),player.get_id());
 if(scene.get_parent(npc.get_id())!=player.get_id()){std::cerr<<"Failed to establish server hierarchy\n";return 1;}
 Engine::Networking::NetworkingRuntime network;
 const Engine::Networking::NetEntityId playerNet{1},npcNet{2};
 network.publish(playerNet,{});network.publish(npcNet,{{2,0,0}});
 network.relevancy().upsert({playerNet,{0,0,0},128,Engine::Networking::RelevancyMode::Always});
 network.relevancy().upsert({npcNet,{2,0,0},128,Engine::Networking::RelevancyMode::Distance});
 const auto connection=network.connections().add("local-dedicated-client",0);network.connections().set_status(connection,Engine::Networking::ConnectionStatus::Connected);network.ownership().assign(playerNet,connection);
 const std::vector<Engine::Networking::NetEntityId> replicated{playerNet,npcNet};
 Engine::Networking::Snapshot latest;
 for(int tick=0;tick<tickCount;++tick){
  auto&t=scene.transformComponents[player.get_id()];t.position.z+=.1f;
  network.publish(playerNet,{t.position,{}, {0,0,.1f*60}});
  network.properties().set<float>(playerNet,1,t.position.z);
  latest=network.make_snapshot(static_cast<Engine::Networking::Tick>(tick+1),tick/60.0,replicated);
 }
 if(tickCount>0&&(latest.tick!=static_cast<unsigned>(tickCount)||latest.entities.size()!=2)){std::cerr<<"Snapshot generation failed\n";return 1;}
 std::cout<<"VulkanEngineServer completed "<<tickCount<<" headless ticks, "<<latest.entities.size()<<" replicated entities.\n";
 return 0;
}
