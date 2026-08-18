#pragma once

#include "../core/uuid/UUID.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

enum class AssetType {
    Unknown, Texture, Mesh, Material, Audio, Skeleton, Animation, Scene, VoxelStructure, Block
};

struct ImportSettings {
    bool generateMipmaps{true};
    bool srgb{true};
    uint32_t textureQuality{80};
    float meshScale{1.0f};
};

struct AssetMetadata {
    UUID id{0, 0};
    AssetType type{AssetType::Unknown};
    std::filesystem::path sourcePath;
    std::filesystem::path cookedPath;
    uint64_t contentHash{};
    uint32_t importerVersion{1};
    bool isCooked{};
    uint32_t width{};
    uint32_t height{};
    uint32_t channels{};
    uint32_t primitiveCount{};
    uint64_t vertexCount{};
    uint64_t indexCount{};
    uint32_t sampleRate{};
    uint32_t audioChannels{};
    float durationSeconds{};
    uint32_t boneCount{};
    uint32_t animationTrackCount{};
    uint32_t animationKeyframeCount{};
    ImportSettings importSettings;
    uint64_t settingsHash{};
};

class AssetRegistry final {
public:
    bool register_asset(AssetMetadata metadata);
    bool remove_asset(UUID id);
    [[nodiscard]] std::optional<AssetMetadata> find(UUID id) const;
    [[nodiscard]] std::optional<UUID> find_id(const std::filesystem::path& sourcePath) const;
    [[nodiscard]] std::vector<AssetMetadata> snapshot() const;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] bool save(const std::filesystem::path& databasePath) const;
    [[nodiscard]] bool load(const std::filesystem::path& databasePath);
    [[nodiscard]] bool set_dependencies(UUID asset, std::vector<UUID> dependencies);
    [[nodiscard]] std::vector<UUID> dependencies_of(UUID asset) const;
    [[nodiscard]] std::vector<UUID> referencers_of(UUID dependency) const;
    [[nodiscard]] std::vector<UUID> unused_assets(const std::vector<UUID>& roots) const;
    static std::string normalized_key(const std::filesystem::path& path);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<UUID, AssetMetadata> assets_;
    std::unordered_map<std::string, UUID> pathToId_;
    std::unordered_map<UUID, std::vector<UUID>> dependencies_;
};

struct ImportRequest {
    std::filesystem::path source;
    std::filesystem::path cookedDirectory;
    uint32_t importerVersion{1};
    ImportSettings settings;
    std::string platform{"generic"};
};

struct DerivedDataKey {
    std::filesystem::path source;
    uint64_t sourceHash{};
    uint64_t settingsHash{};
    uint32_t importerVersion{1};
    std::string platform{"generic"};

    [[nodiscard]] bool operator==(const DerivedDataKey& other) const noexcept = default;
};

struct DerivedDataEntry {
    std::filesystem::path cookedPath;
    uint64_t contentHash{};
    uint64_t sourceHash{};
    uint64_t settingsHash{};
    uint32_t importerVersion{1};
    std::string platform{"generic"};
};

struct DerivedDataStats {
    uint64_t hits{};
    uint64_t misses{};
    uint64_t invalidations{};
    uint64_t evictions{};
};

class DerivedDataCache final {
public:
    DerivedDataCache() = default;
    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;
    void clear();
    [[nodiscard]] std::optional<DerivedDataEntry> find(const DerivedDataKey& key) const;
    void store(DerivedDataKey key, DerivedDataEntry entry);
    bool invalidate_source(const std::filesystem::path& source);
    [[nodiscard]] std::vector<DerivedDataEntry> entries() const;
    [[nodiscard]] DerivedDataStats stats() const;
    [[nodiscard]] bool trim(size_t maxEntries);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, DerivedDataEntry> entries_;
    mutable DerivedDataStats stats_;
};

struct ImportResult {
    bool success{};
    AssetMetadata asset;
    std::string error;
    std::vector<std::filesystem::path> externalReferences;
    explicit operator bool() const noexcept { return success; }
};

class AssetImporter {
public:
    virtual ~AssetImporter() = default;
    [[nodiscard]] virtual bool supports_extension(std::string_view extension) const = 0;
    [[nodiscard]] virtual ImportResult import(const ImportRequest& request) const = 0;
};

class TextureImporter final : public AssetImporter {
public:
    [[nodiscard]] bool supports_extension(std::string_view extension) const override;
    [[nodiscard]] ImportResult import(const ImportRequest& request) const override;
};

class MeshImporter final : public AssetImporter {
public:
    [[nodiscard]] bool supports_extension(std::string_view extension) const override;
    [[nodiscard]] ImportResult import(const ImportRequest& request) const override;
};

class AnimationClipImporter final : public AssetImporter {
public:
    [[nodiscard]] bool supports_extension(std::string_view extension) const override;
    [[nodiscard]] ImportResult import(const ImportRequest& request) const override;
};

class SkeletonImporter final : public AssetImporter {
public:
    [[nodiscard]] bool supports_extension(std::string_view extension) const override;
    [[nodiscard]] ImportResult import(const ImportRequest& request) const override;
};

class AudioImporter final : public AssetImporter {
public:
    [[nodiscard]] bool supports_extension(std::string_view extension) const override;
    [[nodiscard]] ImportResult import(const ImportRequest& request) const override;
};

class BinaryCopyImporter : public AssetImporter {
public:
    BinaryCopyImporter(AssetType type, std::vector<std::string> extensions, std::string cookedExtension);
    [[nodiscard]] bool supports_extension(std::string_view extension) const override;
    [[nodiscard]] ImportResult import(const ImportRequest& request) const override;

private:
    AssetType type_;
    std::vector<std::string> extensions_;
    std::string cookedExtension_;
};

class TextMaterialImporter : public AssetImporter {
public:
    TextMaterialImporter() = default;
    [[nodiscard]] bool supports_extension(std::string_view extension) const override;
    [[nodiscard]] ImportResult import(const ImportRequest& request) const override;
};

class AssetPipeline final {
public:
    explicit AssetPipeline(AssetRegistry& registry) : registry_(registry) {}
    void add_importer(std::unique_ptr<AssetImporter> importer);
    [[nodiscard]] ImportResult import(const ImportRequest& request);

private:
    AssetRegistry& registry_;
    std::vector<std::unique_ptr<AssetImporter>> importers_;
};

class AssetHotReloadService final {
public:
    using ReloadCallback = std::function<void(const AssetMetadata&)>;

    AssetHotReloadService(AssetPipeline& pipeline, AssetRegistry& registry,
                          std::filesystem::path cookedDirectory)
        : pipeline_(pipeline), registry_(registry), cookedDirectory_(std::move(cookedDirectory)) {}

    void set_reload_callback(ReloadCallback callback) { callback_ = std::move(callback); }
    void watch_registered_assets();
    [[nodiscard]] std::vector<AssetMetadata> poll();

private:
    AssetPipeline& pipeline_;
    AssetRegistry& registry_;
    std::filesystem::path cookedDirectory_;
    ReloadCallback callback_;
    std::unordered_map<std::string, std::filesystem::file_time_type> writeTimes_;
};

class AssetPackager final {
public:
    struct Result {
        bool success{};
        std::filesystem::path manifestPath;
        std::vector<UUID> assets;
        std::string error;
        explicit operator bool() const noexcept { return success; }
    };
    [[nodiscard]] static Result package(const AssetRegistry& registry,
                                        const std::vector<UUID>& roots,
                                        const std::filesystem::path& outputDirectory);
};

using AssetPackageResult = AssetPackager::Result;

struct AssetFileOperationResult {
    bool success{};
    AssetMetadata asset;
    std::string error;
    explicit operator bool() const noexcept { return success; }
};

class AssetBrowserModel final {
public:
    explicit AssetBrowserModel(AssetRegistry& registry) : registry_(registry) {}
    [[nodiscard]] std::vector<AssetMetadata> query(
        std::string_view search = {}, std::optional<AssetType> type = std::nullopt) const;
    [[nodiscard]] AssetFileOperationResult move_asset(
        UUID id, const std::filesystem::path& destination);
    [[nodiscard]] AssetFileOperationResult duplicate_asset(
        UUID id, const std::filesystem::path& destination);
    [[nodiscard]] AssetFileOperationResult delete_asset(UUID id);

private:
    AssetRegistry& registry_;
};
} // namespace Engine
