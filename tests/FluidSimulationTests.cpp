// FluidSimulationTests.cpp — Gate for IFluidSimulation (B.3 — shallow water)
#include <cstdio>
#include <cmath>
#include "engine/rendering/IFluidSimulation.hpp"

static int g_p=0, g_f=0;
#define CHECK(c,m) do{if(!(c)){std::printf("  FAIL: %s\n",m);g_f++;}else g_p++;}while(0)
#define CHECK_NEAR(a,b,e,m) do{float _d=std::fabs((a)-(b));if(_d>(e)){std::printf("  FAIL: %s (got %f exp %f)\n",m,(float)(a),(float)(b));g_f++;}else g_p++;}while(0)

using namespace vc::rendering;

int main() {
    std::printf("[fluid] ALL tests starting\n");

    // 1. Config
    std::printf("[fluid] test config\n");
    { FluidConfig c; CHECK(c.validate(), "default valid");
      FluidConfig bad; bad.gridSize=0; CHECK(!bad.validate(), "zero gridSize invalid"); }

    // 2. JSON
    std::printf("[fluid] test JSON\n");
    { FluidConfig c; c.gridSize=32; c.gravity=10; std::string j=c.toJson();
      auto c2=FluidConfig::fromJson(j); CHECK(c2.gridSize==32,"json gridSize"); CHECK_NEAR(c2.gravity,10.0f,0.01f,"json gravity"); }

    // 3. Create state
    std::printf("[fluid] test create\n");
    { std::string e; auto fs=create_fluid_simulation(FluidConfig{},e); auto s=fs->createState();
      CHECK(fs->maxHeight(s)==0,"fresh state zero height"); CHECK(fs->totalVolume(s,fs->getConfig())==0,"fresh zero volume"); }

    // 4. Set/get height
    std::printf("[fluid] test set/get\n");
    { std::string e; auto fs=create_fluid_simulation(FluidConfig{},e); auto s=fs->createState();
      fs->setHeight(s,5,5,2.0f); CHECK_NEAR(fs->getHeight(s,5,5),2.0f,1e-6f,"set height");
      CHECK_NEAR(fs->getHeight(s,0,0),0.0f,1e-6f,"unset is zero");
      CHECK_NEAR(fs->getHeight(s,-1,0),0.0f,1e-6f,"OOB returns zero"); }

    // 5. Add source
    std::printf("[fluid] test addSource\n");
    { std::string e; FluidConfig cfg; cfg.gridSize=8; auto fs=create_fluid_simulation(cfg,e); auto s=fs->createState();
      fs->addSource(s,4,4,1.0f); CHECK_NEAR(fs->getHeight(s,4,4),1.0f,1e-6f,"add source");
      fs->addSource(s,4,4,0.5f); CHECK_NEAR(fs->getHeight(s,4,4),1.5f,1e-6f,"add source again"); }

    // 6. Total volume
    std::printf("[fluid] test volume\n");
    { std::string e; FluidConfig cfg; cfg.gridSize=4; cfg.cellSize=2.0f;
      auto fs=create_fluid_simulation(cfg,e); auto s=fs->createState();
      fs->setHeight(s,0,0,1.0f); fs->setHeight(s,1,0,1.0f);
      CHECK_NEAR(fs->totalVolume(s,cfg),8.0f,0.01f,"volume=2cells*1h*4area"); }

    // 7. Max height
    std::printf("[fluid] test maxHeight\n");
    { std::string e; FluidConfig cfg; cfg.gridSize=4; auto fs=create_fluid_simulation(cfg,e); auto s=fs->createState();
      fs->setHeight(s,1,1,3.0f); fs->setHeight(s,2,2,5.0f);
      CHECK_NEAR(fs->maxHeight(s),5.0f,1e-6f,"max height"); }

    // 8. Simulation: flat water stays flat
    std::printf("[fluid] test flat stable\n");
    { std::string e; FluidConfig cfg; cfg.gridSize=8; cfg.gravity=10;
      auto fs=create_fluid_simulation(cfg,e); auto s=fs->createState();
      for(int x=0;x<8;x++) for(int z=0;z<8;z++) fs->setHeight(s,x,z,1.0f);
      fs->simulate(s,cfg);
      CHECK_NEAR(fs->getHeight(s,4,4),1.0f,0.05f,"flat stays flat"); }

    // 9. Simulation: dam break — water flows from high to low
    std::printf("[fluid] test dam break\n");
    { std::string e; FluidConfig cfg; cfg.gridSize=16; cfg.gravity=10; cfg.cellSize=1.0f;
      auto fs=create_fluid_simulation(cfg,e); auto s=fs->createState();
      for(int x=0;x<8;x++) for(int z=0;z<16;z++) fs->setHeight(s,x,z,5.0f);
      // Add a small perturbation on the high side
      fs->setHeight(s,7,8,5.1f);
      float h_before = fs->getHeight(s,12,8);
      float vol_before = fs->totalVolume(s,cfg);
      for(int i=0;i<100;i++) fs->simulate(s,cfg);
      float vol_after = fs->totalVolume(s,cfg);
      // Volume should be approximately conserved
      CHECK_NEAR(vol_after, vol_before, vol_before*0.1f, "volume conserved");
      // Max height should decrease slightly (energy dissipation)
      CHECK(fs->maxHeight(s) <= 5.11f, "max height bounded"); }

    // 10. Determinism
    std::printf("[fluid] test determinism\n");
    { std::string e; FluidConfig cfg; cfg.gridSize=8;
      auto fs=create_fluid_simulation(cfg,e);
      auto s1=fs->createState(); auto s2=fs->createState();
      fs->setHeight(s1,3,3,2.0f); fs->setHeight(s2,3,3,2.0f);
      fs->simulate(s1,cfg); fs->simulate(s2,cfg);
      CHECK(s1.height==s2.height,"deterministic height"); }

    // 11. Refusals
    std::printf("[fluid] test refusals\n");
    { FluidConfig bad; bad.gridSize=0; std::string e;
      auto p=create_fluid_simulation(bad,e); CHECK(p==nullptr,"bad config refused"); }

    // 12. Config getter
    std::printf("[fluid] test config getter\n");
    { std::string e; FluidConfig cfg; cfg.gravity=15;
      auto fs=create_fluid_simulation(cfg,e); CHECK_NEAR(fs->getConfig().gravity,15.0f,0.001f,"config gravity"); }

    std::printf("\n[fluid] Results: %d passed, %d failed\n",g_p,g_f);
    if(g_f==0) std::printf("[fluid] ALL PASSED\n");
    return g_f==0?0:1;
}
