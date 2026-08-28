#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Assets {

// Thumbnail cache (README §29 "thumbnails/previews"). Generates small RGBA
// previews for assets (textures, meshes, materials), caches them in memory and
// persists them to disk so the editor only regenerates on content change.
class ThumbnailCache final {
public:
    struct Thumbnail {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> rgba;  // width*height*4
        std::uint64_t sourceHash{};
        [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0 && !rgba.empty(); }
    };

    struct Options {
        std::uint32_t size{128};                       // square thumbnails
        std::filesystem::path cacheDirectory;          // persists to <dir>/thumbnails
        bool persist{true};
    };

    ThumbnailCache();
    explicit ThumbnailCache(Options options);

    // Renders (or returns cached) a thumbnail for `assetKey`.
    // `generator` produces RGBA at the requested size; it is only invoked on
    // cache miss. `sourceHash` invalidates stale cache entries.
    [[nodiscard]] std::optional<Thumbnail> get(
        const std::string& assetKey, std::uint64_t sourceHash,
        const std::function<void(std::uint32_t, std::uint32_t, std::vector<std::uint8_t>&)>& generator);

    // Built-in generators for common asset shapes (testable without a GPU).
    static void generate_checkerboard(std::uint32_t size, std::vector<std::uint8_t>& rgba);
    static void generate_gradient(std::uint32_t size, std::vector<std::uint8_t>& rgba);

    // Persistence: saves a thumbnail entry to disk; loads all persisted entries
    // on startup when `persist` is enabled.
    [[nodiscard]] bool save_to_disk(const std::string& assetKey, const Thumbnail& thumb) const;
    [[nodiscard]] bool load_from_disk(const std::string& assetKey, Thumbnail& thumb) const;

    [[nodiscard]] std::size_t memory_entries() const;
    [[nodiscard]] std::size_t memory_bytes() const;
    void clear_memory();

    // Derived cache path for an asset key (stable hash).
    [[nodiscard]] std::filesystem::path disk_path(const std::string& assetKey) const;

private:
    struct Entry {
        Thumbnail thumbnail;
        std::uint64_t lastAccess{};
    };

    Options options_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace Engine::Assets
