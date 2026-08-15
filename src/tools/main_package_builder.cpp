#include "BuildTools.hpp"
#include <iostream>
int main(int argc,char**argv){if(argc<4){std::cerr<<"Usage: VulkanPackageBuilder <executable> <cooked-content> <output> [platform] [configuration]\n";return 2;}Engine::Tools::PackageOptions o;if(argc>4)o.platform=argv[4];if(argc>5)o.configuration=argv[5];auto r=Engine::Tools::PackageBuilder::build(argv[1],argv[2],argv[3],o);std::cout<<r.message<<'\n';return r?0:1;}
