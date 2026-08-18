#include "RuntimePackage.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
namespace Engine{
bool RuntimeAssetPackage::mount(const std::filesystem::path&root,std::string*error){std::ifstream in(root/"AssetManifest.txt");std::string magic;unsigned version{};if(!(in>>magic>>version)||magic!="VulkanEngine.AssetPackage"||version!=1){if(error)*error="Invalid asset package manifest";return false;}std::unordered_map<UUID,PackagedAsset>loaded;std::string idText,path;int type{};uint64_t hash{};while(in>>std::quoted(idText)>>type>>std::quoted(path)>>hash){UUID id=UUID::from_string(idText);if(!id.is_valid()||type<static_cast<int>(AssetType::Unknown)||type>static_cast<int>(AssetType::Block)||loaded.contains(id)){if(error)*error="Invalid asset record";return false;}std::filesystem::path relative(path);if(relative.is_absolute()||relative.string().find("..")!=std::string::npos||!std::filesystem::is_regular_file(root/relative)){if(error)*error="Missing or unsafe packaged asset";return false;}loaded.emplace(id,PackagedAsset{id,static_cast<AssetType>(type),relative,hash});}if(!in.eof()){if(error)*error="Malformed package manifest";return false;}root_=std::filesystem::weakly_canonical(root);assets_=std::move(loaded);return true;}
void RuntimeAssetPackage::unmount(){root_.clear();assets_.clear();}
std::optional<PackagedAsset>RuntimeAssetPackage::find(UUID id)const{auto i=assets_.find(id);return i==assets_.end()?std::nullopt:std::optional<PackagedAsset>(i->second);}
std::filesystem::path RuntimeAssetPackage::absolute_path(UUID id)const{auto a=find(id);return a?root_/a->relativePath:std::filesystem::path{};}
std::vector<PackagedAsset>RuntimeAssetPackage::assets()const{std::vector<PackagedAsset>r;for(auto&[id,a]:assets_)r.push_back(a);std::sort(r.begin(),r.end(),[](auto&a,auto&b){return a.id<b.id;});return r;}
} // namespace Engine
