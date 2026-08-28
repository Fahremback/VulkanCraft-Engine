#pragma once
#include "engine/assets/IAssetFormats.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace engine::assets {
struct CookOptions { std::string platform{"generic"}; std::uint32_t version{1}; bool compress{true}; };
struct CookArtifact { std::string source_name; AssetFormat format{AssetFormat::Text}; std::vector<std::uint8_t> bytes; std::uint64_t content_hash{0}; bool cache_hit{false}; };
class IAssetCooker {
public:
 virtual ~IAssetCooker()=default;
 virtual bool cook(const FormatDocument&,const CookOptions&,CookArtifact&,std::string&)=0;
 virtual bool validate(const CookArtifact&,std::string&)const=0;
 virtual void clear_cache()=0;
};
std::unique_ptr<IAssetCooker> create_asset_cooker();
}
