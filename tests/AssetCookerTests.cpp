#include "engine/assets/IAssetCooker.hpp"
#include <cassert>
#include <iostream>
int main(){auto c=engine::assets::create_asset_cooker();engine::assets::FormatDocument d{engine::assets::AssetFormat::Text,"a.txt",{'o','k'},{{"dep.txt","text"}}};engine::assets::CookArtifact a,b;std::string e;assert(c->cook(d,{},a,e));assert(c->validate(a,e));assert(c->cook(d,{},b,e));assert(b.cache_hit&&b.bytes==a.bytes&&b.content_hash==a.content_hash);std::cout<<"asset-cooker-ok\n";}
