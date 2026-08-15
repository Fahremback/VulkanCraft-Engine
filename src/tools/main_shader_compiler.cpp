#include "BuildTools.hpp"
#include <iostream>
int main(int argc,char**argv){if(argc<5){std::cerr<<"Usage: VulkanShaderCompiler <source> <output.spv> <stage> <entry> [include-dir...]\n";return 2;}Engine::Tools::ShaderCompileOptions o;o.stage=argv[3];o.entry=argv[4];for(int i=5;i<argc;++i)o.includeDirectories.emplace_back(argv[i]);auto r=Engine::Tools::ShaderCompiler::compile(argv[1],argv[2],o);std::cout<<r.message<<'\n';return r?0:1;}
