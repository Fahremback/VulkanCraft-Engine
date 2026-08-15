#pragma once
#include "AssetRegistry.hpp"
#include <filesystem>
#include <optional>
#include <unordered_map>
namespace Engine{
struct PackagedAsset{UUID id;AssetType type{AssetType::Unknown};std::filesystem::path relativePath;uint64_t contentHash{};};
class RuntimeAssetPackage final{
public:
 bool mount(const std::filesystem::path& packageRoot,std::string*error=nullptr);void unmount();bool mounted()const noexcept{return !root_.empty();}
 std::optional<PackagedAsset> find(UUID id)const;std::filesystem::path absolute_path(UUID id)const;std::vector<PackagedAsset> assets()const;
private:std::filesystem::path root_;std::unordered_map<UUID,PackagedAsset>assets_;
};
} // namespace Engine
