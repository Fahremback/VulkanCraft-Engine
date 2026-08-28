#include "ThumbnailCache.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace Engine::Assets {

namespace {
std::uint64_t fnv1a64(const std::string& value) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (char c : value) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}
} // namespace

ThumbnailCache::ThumbnailCache() : ThumbnailCache(Options{}) {}

ThumbnailCache::ThumbnailCache(Options options) : options_(std::move(options)) {}

std::optional<ThumbnailCache::Thumbnail> ThumbnailCache::get(
    const std::string& assetKey, std::uint64_t sourceHash,
    const std::function<void(std::uint32_t, std::uint32_t, std::vector<std::uint8_t>&)>& generator) {
    if (assetKey.empty() || !generator) return std::nullopt;

    // The source hash identifies the asset's *content*, not the renderer that
    // produced the thumbnail. Mix in a renderer version so fixing/improving a
    // thumbnail generator invalidates every stale preview (memory + disk)
    // instead of returning the old broken image forever.
    constexpr std::uint64_t kThumbnailRendererVersion = 2;
    const std::uint64_t effectiveHash =
        sourceHash ^ (kThumbnailRendererVersion * 0x9E3779B97F4A7C15ull);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(assetKey);
        if (it != entries_.end() && it->second.thumbnail.sourceHash == effectiveHash) {
            it->second.lastAccess = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            return it->second.thumbnail;
        }
    }

    // Disk fallback (persisted cache): only if hashes match.
    Thumbnail diskThumb;
    if (options_.persist && load_from_disk(assetKey, diskThumb) && diskThumb.sourceHash == effectiveHash) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[assetKey] = {diskThumb, 0};
        return diskThumb;
    }

    // Generate.
    Thumbnail thumb;
    thumb.width = options_.size;
    thumb.height = options_.size;
    generator(thumb.width, thumb.height, thumb.rgba);
    if (thumb.rgba.size() != static_cast<std::size_t>(thumb.width) * thumb.height * 4) return std::nullopt;
    thumb.sourceHash = effectiveHash;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[assetKey] = {thumb, static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count())};
    }
    if (options_.persist) save_to_disk(assetKey, thumb);
    return thumb;
}

void ThumbnailCache::generate_checkerboard(std::uint32_t size, std::vector<std::uint8_t>& rgba) {
    rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    const std::uint32_t cell = size / 8 > 0 ? size / 8 : 1;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const bool dark = ((x / cell) + (y / cell)) % 2 == 0;
            const std::size_t o = (static_cast<std::size_t>(y) * size + x) * 4;
            rgba[o + 0] = dark ? 32 : 220;
            rgba[o + 1] = dark ? 32 : 220;
            rgba[o + 2] = dark ? 32 : 220;
            rgba[o + 3] = 255;
        }
    }
}

void ThumbnailCache::generate_gradient(std::uint32_t size, std::vector<std::uint8_t>& rgba) {
    rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * size + x) * 4;
            rgba[o + 0] = static_cast<std::uint8_t>((x * 255) / std::max(1u, size - 1));
            rgba[o + 1] = static_cast<std::uint8_t>((y * 255) / std::max(1u, size - 1));
            rgba[o + 2] = 128;
            rgba[o + 3] = 255;
        }
    }
}

std::filesystem::path ThumbnailCache::disk_path(const std::string& assetKey) const {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(fnv1a64(assetKey)));
    return options_.cacheDirectory / (std::string(buffer) + ".thumb");
}

bool ThumbnailCache::save_to_disk(const std::string& assetKey, const Thumbnail& thumb) const {
    if (options_.cacheDirectory.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(options_.cacheDirectory, ec);
    if (ec) return false;
    std::ofstream out(disk_path(assetKey), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::uint32_t w = thumb.width, h = thumb.height;
    const std::uint64_t hash = thumb.sourceHash;
    out.write(reinterpret_cast<const char*>(&w), sizeof(w));
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
    const std::uint32_t count = static_cast<std::uint32_t>(thumb.rgba.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    out.write(reinterpret_cast<const char*>(thumb.rgba.data()), static_cast<std::streamsize>(thumb.rgba.size()));
    return out.good();
}

bool ThumbnailCache::load_from_disk(const std::string& assetKey, Thumbnail& thumb) const {
    std::ifstream in(disk_path(assetKey), std::ios::binary);
    if (!in) return false;
    std::uint32_t w{}, h{}, count{};
    std::uint64_t hash{};
    in.read(reinterpret_cast<char*>(&w), sizeof(w));
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    in.read(reinterpret_cast<char*>(&hash), sizeof(hash));
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in || w == 0 || h == 0 || count != static_cast<std::uint64_t>(w) * h * 4) return false;
    thumb.width = w;
    thumb.height = h;
    thumb.sourceHash = hash;
    thumb.rgba.resize(count);
    in.read(reinterpret_cast<char*>(thumb.rgba.data()), static_cast<std::streamsize>(count));
    if (!in.good() && !in.eof()) return false;
    return thumb.rgba.size() == count;
}

std::size_t ThumbnailCache::memory_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::size_t ThumbnailCache::memory_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const auto& [_, entry] : entries_) total += entry.thumbnail.rgba.size();
    return total;
}

void ThumbnailCache::clear_memory() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

} // namespace Engine::Assets
