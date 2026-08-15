#include "BuildTools.hpp"
#include <iostream>
int main(int argc,char**argv){if(argc<4){std::cerr<<"Usage: VulkanProjectGenerator <directory> <name> <engine-path> [--voxel]\n";return 2;}Engine::Tools::ProjectOptions o;o.name=argv[2];o.enginePath=argv[3];o.voxelPlugin=argc>4&&std::string(argv[4])=="--voxel";auto r=Engine::Tools::ProjectGenerator::generate(argv[1],o);std::cout<<r.message<<'\n';return r?0:1;}
