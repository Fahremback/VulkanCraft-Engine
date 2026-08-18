#include "AssetRegistry.hpp"
#include "GltfGeometry.hpp"
#include "ExrDecoder.hpp"
#include "FbxImporter.hpp"
#include "../rendering/materials/Material.hpp"
#include "../animation/AnimationAssets.hpp"
#include "../audio/OggDecoder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <regex>
#include <span>
#include <sstream>
#include <unordered_set>

namespace Engine {
namespace {

uint64_t hash_file(const std::filesystem::path& path, uint32_t importerVersion);

uint64_t settings_hash(const ImportSettings& settings) {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    uint32_t meshScaleBits{};
    static_assert(sizeof(meshScaleBits) == sizeof(settings.meshScale));
    std::memcpy(&meshScaleBits, &settings.meshScale, sizeof(meshScaleBits));
    mix(settings.generateMipmaps ? 1u : 0u);
    mix(settings.srgb ? 1u : 0u);
    mix(settings.textureQuality);
    mix(meshScaleBits);
    return hash;
}

uint64_t cooked_hash(const ImportRequest& request) {
    const uint64_t settings = settings_hash(request.settings);
    return hash_file(request.source, request.importerVersion) ^ (settings + 0x9e3779b97f4a7c15ull);
}

void apply_import_settings(AssetMetadata& asset, const ImportRequest& request) {
    asset.importSettings = request.settings;
    asset.settingsHash = settings_hash(request.settings);
    asset.contentHash = cooked_hash(request);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Import-settings transforms (Fase 2). Both operate on decoded 8-bit RGB/RGBA
// payloads only: textureQuality box-downscales on a ladder (>=80 full res,
// >=40 half, >=20 quarter, >=10 eighth, else 1/16) and generateMipmaps appends
// a full CPU mip chain (halving box filter down to 1x1). PNG keeps its raw
// bytes and HDR (half-float) keeps a single level.
struct CookedTextureLevel {
    uint32_t width{ 0 };
    uint32_t height{ 0 };
    std::vector<uint8_t> pixels;
};

uint32_t quality_downscale_levels(uint32_t quality) {
    if (quality >= 80) return 0;
    if (quality >= 40) return 1;
    if (quality >= 20) return 2;
    if (quality >= 10) return 3;
    return 4;
}

CookedTextureLevel box_downscale(const uint8_t* src, uint32_t w, uint32_t h, uint32_t channels) {
    const uint32_t w2 = std::max(w / 2u, 1u);
    const uint32_t h2 = std::max(h / 2u, 1u);
    CookedTextureLevel out;
    out.width = w2;
    out.height = h2;
    out.pixels.resize(static_cast<size_t>(w2) * h2 * channels);
    for (uint32_t y = 0; y < h2; ++y) {
        for (uint32_t x = 0; x < w2; ++x) {
            uint32_t sum[4] = { 0, 0, 0, 0 };
            uint32_t count = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) {
                const uint32_t sy = std::min(y * 2 + dy, h - 1);
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const uint32_t sx = std::min(x * 2 + dx, w - 1);
                    const uint8_t* p = src + (static_cast<size_t>(sy) * w + sx) * channels;
                    for (uint32_t c = 0; c < channels; ++c) sum[c] += p[c];
                    ++count;
                }
            }
            uint8_t* d = out.pixels.data() + (static_cast<size_t>(y) * w2 + x) * channels;
            for (uint32_t c = 0; c < channels; ++c) d[c] = static_cast<uint8_t>(sum[c] / count);
        }
    }
    return out;
}

uint64_t hash_file(const std::filesystem::path& path, uint32_t importerVersion) {
    constexpr uint64_t offset = 14695981039346656037ull;
    constexpr uint64_t prime = 1099511628211ull;
    uint64_t hash = offset;
    std::ifstream input(path, std::ios::binary);
    char bytes[64 * 1024];
    while (input) {
        input.read(bytes, sizeof(bytes));
        for (std::streamsize i = 0; i < input.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(bytes[i]);
            hash *= prime;
        }
    }
    hash ^= importerVersion;
    hash *= prime;
    return hash;
}

} // namespace

std::string AssetRegistry::normalized_key(const std::filesystem::path& path) {
    return lower(path.lexically_normal().generic_string());
}

bool AssetRegistry::register_asset(AssetMetadata metadata) {
    if (!metadata.id.is_valid() || metadata.sourcePath.empty()) return false;
    std::unique_lock lock(mutex_);
    const std::string key = normalized_key(metadata.sourcePath);
    if (auto old = assets_.find(metadata.id); old != assets_.end())
        pathToId_.erase(normalized_key(old->second.sourcePath));
    if (auto collision = pathToId_.find(key);
        collision != pathToId_.end() && collision->second != metadata.id) return false;
    pathToId_[key] = metadata.id;
    assets_.insert_or_assign(metadata.id, std::move(metadata));
    return true;
}

bool AssetRegistry::remove_asset(UUID id) {
    std::unique_lock lock(mutex_);
    auto found = assets_.find(id);
    if (found == assets_.end()) return false;
    pathToId_.erase(normalized_key(found->second.sourcePath));
    assets_.erase(found);
    dependencies_.erase(id);
    for (auto& [owner, dependencies] : dependencies_) {
        dependencies.erase(std::remove(dependencies.begin(), dependencies.end(), id), dependencies.end());
    }
    return true;
}

std::optional<AssetMetadata> AssetRegistry::find(UUID id) const {
    std::shared_lock lock(mutex_);
    auto found = assets_.find(id);
    if (found == assets_.end()) return std::nullopt;
    return found->second;
}

std::optional<UUID> AssetRegistry::find_id(const std::filesystem::path& path) const {
    std::shared_lock lock(mutex_);
    auto found = pathToId_.find(normalized_key(path));
    if (found == pathToId_.end()) return std::nullopt;
    return found->second;
}

std::vector<AssetMetadata> AssetRegistry::snapshot() const {
    std::shared_lock lock(mutex_);
    std::vector<AssetMetadata> result;
    result.reserve(assets_.size());
    for (const auto& [id, asset] : assets_) result.push_back(asset);
    return result;
}

size_t AssetRegistry::size() const {
    std::shared_lock lock(mutex_);
    return assets_.size();
}

bool AssetRegistry::set_dependencies(UUID asset, std::vector<UUID> dependencies) {
    std::unique_lock lock(mutex_);
    if (!assets_.contains(asset)) return false;
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    if (std::find(dependencies.begin(), dependencies.end(), asset) != dependencies.end()) return false;
    for (UUID dependency : dependencies)
        if (!assets_.contains(dependency)) return false;
    dependencies_[asset] = std::move(dependencies);
    return true;
}

std::vector<UUID> AssetRegistry::dependencies_of(UUID asset) const {
    std::shared_lock lock(mutex_);
    const auto found = dependencies_.find(asset);
    return found == dependencies_.end() ? std::vector<UUID>{} : found->second;
}

std::vector<UUID> AssetRegistry::referencers_of(UUID dependency) const {
    std::shared_lock lock(mutex_);
    std::vector<UUID> result;
    for (const auto& [owner, dependencies] : dependencies_)
        if (std::binary_search(dependencies.begin(), dependencies.end(), dependency)) result.push_back(owner);
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<UUID> AssetRegistry::unused_assets(const std::vector<UUID>& roots) const {
    std::shared_lock lock(mutex_);
    std::unordered_set<UUID> reachable;
    std::vector<UUID> pending;
    for (UUID root : roots) {
        if (assets_.contains(root) && reachable.insert(root).second) pending.push_back(root);
    }
    while (!pending.empty()) {
        const UUID current = pending.back();
        pending.pop_back();
        const auto found = dependencies_.find(current);
        if (found == dependencies_.end()) continue;
        for (UUID dependency : found->second)
            if (reachable.insert(dependency).second) pending.push_back(dependency);
    }
    std::vector<UUID> unused;
    unused.reserve(assets_.size() - reachable.size());
    for (const auto& [id, asset] : assets_)
        if (!reachable.contains(id)) unused.push_back(id);
    std::sort(unused.begin(), unused.end());
    return unused;
}

bool AssetRegistry::save(const std::filesystem::path& databasePath) const {
    std::error_code error;
    if (databasePath.has_parent_path()) std::filesystem::create_directories(databasePath.parent_path(), error);
    if (error) return false;
    std::ofstream output(databasePath, std::ios::trunc);
    if (!output) return false;
    output << "VulkanEngine.AssetRegistry 7\n";
    for (const AssetMetadata& asset : snapshot()) {
        const std::vector<UUID> dependencies = dependencies_of(asset.id);
        output << std::quoted(asset.id.to_string()) << ' '
               << static_cast<int>(asset.type) << ' '
               << std::quoted(asset.sourcePath.generic_string()) << ' '
               << std::quoted(asset.cookedPath.generic_string()) << ' '
               << asset.contentHash << ' ' << asset.importerVersion << ' '
               << static_cast<int>(asset.isCooked) << ' '
               << asset.width << ' ' << asset.height << ' ' << asset.channels << ' '
               << asset.primitiveCount << ' ' << asset.vertexCount << ' ' << asset.indexCount << ' '
               << asset.sampleRate << ' ' << asset.audioChannels << ' ' << asset.durationSeconds << ' '
               << asset.boneCount << ' ' << asset.animationTrackCount << ' ' << asset.animationKeyframeCount << ' '
               << asset.settingsHash << ' '
               << static_cast<int>(asset.importSettings.generateMipmaps) << ' '
               << static_cast<int>(asset.importSettings.srgb) << ' '
               << asset.importSettings.textureQuality << ' ' << asset.importSettings.meshScale << ' '
               << dependencies.size();
        for (UUID dependency : dependencies) output << ' ' << std::quoted(dependency.to_string());
        output << '\n';
    }
    return output.good();
}

bool AssetRegistry::load(const std::filesystem::path& databasePath) {
    std::ifstream input(databasePath);
    if (!input) return false;
    std::string format;
    unsigned version{};
    if (!(input >> format >> version) || format != "VulkanEngine.AssetRegistry") return false;

    // Migration: versions 1..6 used a progressively smaller field set.
    // Field counts per version (after the 3 fixed tokens: quotedId type quotedSource):
    //   v1: sourcePath cookedPath hash importerVersion cooked (no metadata fields)
    //   v2: + width height channels primitiveCount
    //   v3: + vertexCount indexCount
    //   v4: + sampleRate audioChannels durationSeconds
    //   v5: + boneCount animationTrackCount animationKeyframeCount
    //   v6: + settingsHash generateMipmaps srgb
    //   v7: + textureQuality meshScale
    // Every older line is padded with defaults for the missing trailing fields.
    if (version < 1 || version > 7) return false;

    std::unordered_map<UUID, AssetMetadata> loadedAssets;
    std::unordered_map<std::string, UUID> loadedPaths;
    std::unordered_map<UUID, std::vector<UUID>> loadedDependencies;
    std::string idText, sourceText, cookedText;
    int type{}, cooked{}, generateMipmaps{}, srgb{};
    uint64_t hash{}, settingsHash{};
    uint32_t importerVersion{}, width{}, height{}, channels{}, primitiveCount{}, sampleRate{}, audioChannels{}, textureQuality{};
    uint32_t boneCount{}, animationTrackCount{}, animationKeyframeCount{};
    uint64_t vertexCount{}, indexCount{};
    float durationSeconds{}, meshScale{};
    size_t dependencyCount{};

    auto parse_line = [&]() -> bool {
        if (version == 7) {
            if (!(input >> std::quoted(idText) >> type >> std::quoted(sourceText) >>
                  std::quoted(cookedText) >> hash >> importerVersion >> cooked >>
                  width >> height >> channels >> primitiveCount >> vertexCount >> indexCount >>
                  sampleRate >> audioChannels >> durationSeconds >> boneCount >> animationTrackCount >> animationKeyframeCount >>
                  settingsHash >> generateMipmaps >> srgb >>
                  textureQuality >> meshScale >> dependencyCount)) return false;
            return true;
        }
        // Legacy lines: read only the fields that existed, default the rest.
        if (!(input >> std::quoted(idText) >> type >> std::quoted(sourceText) >>
              std::quoted(cookedText) >> hash >> importerVersion >> cooked)) return false;
        width = height = channels = primitiveCount = 0;
        vertexCount = indexCount = 0;
        sampleRate = audioChannels = 0; durationSeconds = 0.0f;
        boneCount = animationTrackCount = animationKeyframeCount = 0;
        settingsHash = 0; generateMipmaps = 0; srgb = 0;
        textureQuality = 100; meshScale = 1.0f;
        if (version >= 2) {
            if (!(input >> width >> height >> channels >> primitiveCount)) return false;
            if (version >= 3) {
                if (!(input >> vertexCount >> indexCount)) return false;
                if (version >= 4) {
                    if (!(input >> sampleRate >> audioChannels >> durationSeconds)) return false;
                    if (version >= 5) {
                        if (!(input >> boneCount >> animationTrackCount >> animationKeyframeCount)) return false;
                        if (version >= 6) {
                            if (!(input >> settingsHash >> generateMipmaps >> srgb)) return false;
                        }
                    }
                }
            }
        }
        if (!(input >> dependencyCount)) return false;
        return true;
    };

    while (parse_line()) {
        if (type < static_cast<int>(AssetType::Unknown) || type > static_cast<int>(AssetType::Block) ||
            (cooked != 0 && cooked != 1) || (generateMipmaps != 0 && generateMipmaps != 1) ||
            (srgb != 0 && srgb != 1) || textureQuality > 100 || meshScale <= 0.0f ||
            dependencyCount > 100000) return false;
        AssetMetadata asset;
        asset.id = UUID::from_string(idText);
        if (!asset.id.is_valid()) return false;
        asset.type = static_cast<AssetType>(type);
        asset.sourcePath = sourceText;
        asset.cookedPath = cookedText;
        asset.contentHash = hash;
        asset.importerVersion = importerVersion;
        asset.isCooked = cooked != 0;
        asset.width = width;
        asset.height = height;
        asset.channels = channels;
        asset.primitiveCount = primitiveCount;
        asset.vertexCount = vertexCount;
        asset.indexCount = indexCount;
        asset.sampleRate = sampleRate;
        asset.audioChannels = audioChannels;
        asset.durationSeconds = durationSeconds;
        asset.boneCount = boneCount;
        asset.animationTrackCount = animationTrackCount;
        asset.animationKeyframeCount = animationKeyframeCount;
        asset.settingsHash = settingsHash;
        asset.importSettings.generateMipmaps = generateMipmaps != 0;
        asset.importSettings.srgb = srgb != 0;
        asset.importSettings.textureQuality = textureQuality;
        asset.importSettings.meshScale = meshScale;
        const std::string key = normalized_key(asset.sourcePath);
        if (key.empty() || loadedAssets.contains(asset.id) || loadedPaths.contains(key)) return false;
        std::vector<UUID> dependencies;
        dependencies.reserve(dependencyCount);
        for (size_t i = 0; i < dependencyCount; ++i) {
            std::string dependencyText;
            if (!(input >> std::quoted(dependencyText))) return false;
            UUID dependency = UUID::from_string(dependencyText);
            if (!dependency.is_valid() || dependency == asset.id) return false;
            dependencies.push_back(dependency);
        }
        std::sort(dependencies.begin(), dependencies.end());
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
        loadedDependencies.emplace(asset.id, std::move(dependencies));
        loadedPaths.emplace(key, asset.id);
        loadedAssets.emplace(asset.id, std::move(asset));
    }
    if (!input.eof()) return false;
    for (const auto& [owner, dependencies] : loadedDependencies)
        for (UUID dependency : dependencies)
            if (!loadedAssets.contains(dependency)) return false;
    std::unique_lock lock(mutex_);
    assets_ = std::move(loadedAssets);
    pathToId_ = std::move(loadedPaths);
    dependencies_ = std::move(loadedDependencies);
    return true;
}

namespace {
// Decode a TGA (uncompressed true-color) into tightly packed RGB/RGBA bytes.
struct DecodedImage {
    uint32_t width{};
    uint32_t height{};
    uint32_t channels{};
    uint8_t bitDepth{};
    std::vector<uint8_t> pixels;
};

// IEEE 754 half-precision conversion (round-to-nearest-even).
uint16_t float_to_half(float value) {
    const uint32_t bits = *reinterpret_cast<uint32_t*>(&value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        const uint32_t halfMantissa = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        uint32_t result = halfMantissa;
        if (remainder > halfway || (remainder == halfway && (halfMantissa & 1u))) ++result;
        return static_cast<uint16_t>(sign | result);
    }
    if (exponent >= 31) {
        if (exponent > 31 || (exponent == 31 && mantissa != 0)) return static_cast<uint16_t>(sign | 0x7E00u); // NaN
        return static_cast<uint16_t>(sign | 0x7C00u); // Inf
    }
    uint32_t halfMantissa = mantissa >> 13;
    const uint32_t remainder = mantissa & 0x1FFFu;
    const uint32_t halfway = 0x1000u;
    if (remainder > halfway || (remainder == halfway && (halfMantissa & 1u))) ++halfMantissa;
    if (halfMantissa & 0x400u) { // mantissa overflow -> bump exponent
        halfMantissa = 0;
        ++exponent;
        if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | halfMantissa);
}

bool decode_tga(std::span<const uint8_t> bytes, DecodedImage& out) {
    if (bytes.size() < 18) return false;
    const uint8_t imageType = bytes[2];
    // Types 2 (true-color) and 3 (grayscale) uncompressed, 10/11 RLE variants.
    if (imageType != 2 && imageType != 3 && imageType != 10 && imageType != 11) return false;
    const uint16_t width = static_cast<uint16_t>(bytes[12]) | (static_cast<uint16_t>(bytes[13]) << 8);
    const uint16_t height = static_cast<uint16_t>(bytes[14]) | (static_cast<uint16_t>(bytes[15]) << 8);
    const uint8_t depth = bytes[16];
    if (width == 0 || height == 0) return false;
    const bool rle = (imageType == 10 || imageType == 11);
    const uint32_t channels = (depth == 32 || depth == 24) ? (depth / 8) : 1;
    if (channels == 0) return false;

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(width) * height * channels);
    size_t offset = 18;
    if (rle) {
        while (raw.size() < static_cast<size_t>(width) * height * channels && offset < bytes.size()) {
            const uint8_t packet = bytes[offset++];
            const size_t count = static_cast<size_t>(packet & 0x7F) + 1;
            if (packet & 0x80) {
                if (offset + channels > bytes.size()) return false;
                for (size_t i = 0; i < count; ++i)
                    for (uint32_t c = 0; c < channels; ++c) raw.push_back(bytes[offset + c]);
                offset += channels;
            } else {
                const size_t need = count * channels;
                if (offset + need > bytes.size()) return false;
                raw.insert(raw.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                           bytes.begin() + static_cast<std::ptrdiff_t>(offset + need));
                offset += need;
            }
        }
    } else {
        const size_t rowBytes = static_cast<size_t>(width) * channels;
        const size_t expected = rowBytes * height;
        if (bytes.size() < 18 + expected) return false;
        raw.assign(bytes.begin() + 18, bytes.begin() + 18 + static_cast<std::ptrdiff_t>(expected));
    }
    if (raw.size() < static_cast<size_t>(width) * height * channels) return false;

    // TGA stores bottom-up BGR(A). Convert to top-down RGB(A) for the cooked payload.
    out.width = width;
    out.height = height;
    out.channels = channels;
    out.bitDepth = depth;
    out.pixels.resize(static_cast<size_t>(width) * height * channels);
    const size_t rowBytes = static_cast<size_t>(width) * channels;
    for (uint32_t y = 0; y < height; ++y) {
        const size_t srcRow = static_cast<size_t>(height - 1 - y) * rowBytes;
        const size_t dstRow = static_cast<size_t>(y) * rowBytes;
        for (size_t x = 0; x < rowBytes; x += channels) {
            out.pixels[dstRow + x] = raw[srcRow + x + 2];     // R
            out.pixels[dstRow + x + 1] = raw[srcRow + x + 1]; // G
            out.pixels[dstRow + x + 2] = raw[srcRow + x];     // B
            if (channels == 4) out.pixels[dstRow + x + 3] = raw[srcRow + x + 3];
        }
    }
    return true;
}

// Decode Radiance HDR (RGBE) into float-based 16-bit half-like payload (stored as RGBA16F byte pairs).
bool decode_hdr(std::span<const uint8_t> bytes, DecodedImage& out) {
    if (bytes.size() < 32) return false;
    const std::string_view header(reinterpret_cast<const char*>(bytes.data()), 10);
    if (header != "#?RADIANCE" && header != "#?RGBE") return false;
    size_t pos = 10;
    std::string line;
    auto nextLine = [&]() -> bool {
        line.clear();
        while (pos < bytes.size() && bytes[pos] != '\n') line.push_back(static_cast<char>(bytes[pos++]));
        if (pos >= bytes.size()) return false;
        ++pos; // consume '\n'
        return true;
    };
    while (nextLine()) {
        if (line == "FORMAT=32-bit_rle_rgbe") continue;
        if (line.starts_with("EXPOSURE=") || line.starts_with("GAMMA=") || line.empty()) continue;
        // "Y height X width" orientation line (optionally with +- signs)
        std::istringstream parse(line);
        char axis1{}, axis2{};
        int value1{}, value2{};
        bool validLine = false;
        if (parse >> axis1) {
            if (axis1 == '+' || axis1 == '-') parse >> axis1; // consume sign, then axis letter
            if (parse >> value1 >> axis2) {
                if (axis2 == '+' || axis2 == '-') parse >> axis2;
                if (parse >> value2) validLine = true;
            }
        }
        if (validLine) {
            if (axis1 == 'Y' && axis2 == 'X') {
                out.height = static_cast<uint32_t>(value1);
                out.width = static_cast<uint32_t>(value2);
            } else if (axis1 == 'X' && axis2 == 'Y') {
                out.width = static_cast<uint32_t>(value1);
                out.height = static_cast<uint32_t>(value2);
            } else {
                return false;
            }
            break;
        }
    }
    if (out.width == 0 || out.height == 0 || pos >= bytes.size()) return false;
    out.channels = 4;
    out.bitDepth = 32; // RGBA16F
    out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4 * 2);

    auto readByte = [&]() -> std::optional<uint8_t> {
        if (pos >= bytes.size()) return std::nullopt;
        return bytes[pos++];
    };
    // Simple non-RLE fallback plus old-style RLE support.
    for (uint32_t y = 0; y < out.height; ++y) {
        size_t dstRow = static_cast<size_t>(y) * out.width * 8;
        for (uint32_t x = 0; x < out.width; ++x) {
            uint8_t rgbe[4];
            for (int c = 0; c < 4; ++c) {
                auto b = readByte();
                if (!b) return false;
                rgbe[c] = *b;
            }
            // Convert RGBE to float RGB.
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (rgbe[3] != 0) {
                const float scale = std::ldexp(1.0f, static_cast<int>(rgbe[3]) - (128 + 8));
                r = static_cast<float>(rgbe[0]) * scale;
                g = static_cast<float>(rgbe[1]) * scale;
                b = static_cast<float>(rgbe[2]) * scale;
            }
            const uint16_t halfR = float_to_half(r), halfG = float_to_half(g), halfB = float_to_half(b), halfA = 0x3C00;
            const size_t dst = dstRow + static_cast<size_t>(x) * 8;
            std::memcpy(out.pixels.data() + dst, &halfR, 2);
            std::memcpy(out.pixels.data() + dst + 2, &halfG, 2);
            std::memcpy(out.pixels.data() + dst + 4, &halfB, 2);
            std::memcpy(out.pixels.data() + dst + 6, &halfA, 2);
        }
    }
    return true;
}

// ─── Baseline JPEG decoder (Huffman + DCT) ───
// Decodes baseline sequential JPEG (SOF0): grayscale (1 component) and YCbCr
// with 4:4:4 / 4:2:2 / 4:2:0 sampling. Produces top-down RGB8 pixels, the
// same cooked shape as TGA (raw RGB payload, bitDepth 8, channels 3).

struct JpegHuffmanTable {
    uint8_t counts[16]{};
    uint8_t symbols[256]{};
    int minCode[16]{};
    int maxCode[16]{};
    int valPtr[16]{};
};

struct JpegQuantTable {
    uint8_t data[64]{};
};

struct JpegComponent {
    uint8_t id{};
    uint8_t hSamp{ 1 };
    uint8_t vSamp{ 1 };
    uint8_t qTable{};
    uint8_t dcTable{};
    uint8_t acTable{};
    uint32_t planeW{};
    uint32_t planeH{};
    std::vector<float> plane; // dequantized, post-IDCT samples
};

struct JpegBitReader {
    const uint8_t* data{ nullptr };
    size_t size{};
    size_t pos{};
    int bitCnt{ 0 };
    uint32_t buf{ 0 };

    bool fill() {
        while (bitCnt <= 24) {
            if (pos >= size) return bitCnt > 0;
            uint8_t b = data[pos++];
            if (b == 0xFF) {
                if (pos >= size) return bitCnt > 0;
                const uint8_t m = data[pos];
                if (m == 0x00) {
                    ++pos; // byte-stuffed 0xFF
                } else if (m >= 0xD0 && m <= 0xD7) {
                    ++pos;  // restart marker: realign
                    bitCnt = 0;
                    buf = 0;
                    continue;
                } else {
                    // End of the entropy stream (EOI or a following marker):
                    // keep the bits already buffered; a genuine shortfall
                    // (bitCnt == 0) is still a decode error.
                    return bitCnt > 0;
                }
            }
            buf = (buf << 8) | b;
            bitCnt += 8;
        }
        return true;
    }

    bool get_bits(int n, int* out) {
        if (n == 0) { *out = 0; return true; }
        if (bitCnt < n && !fill()) return false;
        bitCnt -= n;
        *out = static_cast<int>((buf >> bitCnt) & ((1u << n) - 1u));
        return true;
    }
};

void jpeg_build_huffman(JpegHuffmanTable& table) {
    int code = 0, idx = 0;
    for (int i = 0; i < 16; ++i) {
        table.minCode[i] = (table.counts[i] > 0) ? code : -1;
        table.maxCode[i] = -1;
        table.valPtr[i] = idx;
        for (int c = 0; c < table.counts[i]; ++c) {
            ++code;
            ++idx;
        }
        if (table.counts[i] > 0) table.maxCode[i] = code - 1;
        code <<= 1;
    }
}

int jpeg_decode_huffman(JpegBitReader& reader, const JpegHuffmanTable& table) {
    int code = 0;
    for (int i = 0; i < 16; ++i) {
        int bit = 0;
        if (!reader.get_bits(1, &bit)) return -1;
        code = (code << 1) | bit;
        if (table.maxCode[i] >= 0 && code <= table.maxCode[i]) {
            const int index = table.valPtr[i] + (code - table.minCode[i]);
            return table.symbols[static_cast<size_t>(index) & 0xFF];
        }
    }
    return -1;
}

int jpeg_extend(int value, int size) {
    if (value < (1 << (size - 1))) return value - (1 << size) + 1;
    return value;
}

void jpeg_idct_8x8(const float* in, float* out) {
    // 1D IDCT basis: cos((2x+1)*u*pi/16), u = 0 scaled by 1/sqrt(2).
    static const float kCos[8][8] = {
        { 0.70710678f, 0.98078528f, 0.92387953f, 0.83146961f, 0.70710678f, 0.55557023f, 0.38268343f, 0.19509032f },
        { 0.70710678f, 0.83146961f, 0.38268343f, -0.19509032f, -0.70710678f, -0.98078528f, -0.92387953f, -0.55557023f },
        { 0.70710678f, 0.55557023f, -0.38268343f, -0.98078528f, -0.70710678f, 0.19509032f, 0.92387953f, 0.83146961f },
        { 0.70710678f, 0.19509032f, -0.92387953f, -0.55557023f, 0.70710678f, 0.83146961f, -0.38268343f, -0.98078528f },
        { 0.70710678f, -0.19509032f, -0.92387953f, 0.55557023f, 0.70710678f, -0.83146961f, -0.38268343f, 0.98078528f },
        { 0.70710678f, -0.55557023f, -0.38268343f, 0.98078528f, -0.70710678f, -0.19509032f, 0.92387953f, -0.83146961f },
        { 0.70710678f, -0.83146961f, 0.38268343f, 0.19509032f, -0.70710678f, 0.98078528f, -0.92387953f, 0.55557023f },
        { 0.70710678f, -0.98078528f, 0.92387953f, -0.83146961f, 0.70710678f, -0.55557023f, 0.38268343f, -0.19509032f },
    };
    float tmp[64];
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            float sum = 0.0f;
            for (int u = 0; u < 8; ++u) sum += kCos[x][u] * in[u + y * 8];
            tmp[x + y * 8] = sum * 0.5f;
        }
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            float sum = 0.0f;
            for (int v = 0; v < 8; ++v) sum += kCos[y][v] * tmp[x + v * 8];
            out[x + y * 8] = sum * 0.5f;
        }
    }
}

bool decode_jpeg(std::span<const uint8_t> bytes, DecodedImage& out) {
    // ── Marker scan ──
    static constexpr uint8_t kZigzag[64] = {
         0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    };
    if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) return false;

    JpegQuantTable quant[4]{};
    JpegHuffmanTable huffDc[4]{}, huffAc[4]{};
    std::vector<JpegComponent> components;
    uint16_t width = 0, height = 0;
    bool haveFrame = false, haveScan = false;

    size_t pos = 2;
    const auto readU16 = [&](size_t at) -> uint16_t {
        return static_cast<uint16_t>((static_cast<uint16_t>(bytes[at]) << 8) | bytes[at + 1]);
    };

    while (pos + 1 < bytes.size()) {
        if (bytes[pos] != 0xFF) return false;
        const uint8_t marker = bytes[pos + 1];
        if (marker == 0xD9) break; // EOI
        pos += 2;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; // standalone
        if (pos + 1 >= bytes.size()) return false;
        const uint16_t length = readU16(pos);
        if (length < 2 || pos + length > bytes.size()) return false;
        const size_t end = pos + length;
        size_t p = pos + 2;

        if (marker == 0xC0) { // SOF0 (baseline)
            if (bytes[p] != 8) return false; // only 8-bit precision
            height = readU16(p + 1);
            width = readU16(p + 3);
            const uint8_t compCount = bytes[p + 5];
            if (width == 0 || height == 0 || compCount == 0 || compCount > 4) return false;
            components.resize(compCount);
            for (uint8_t i = 0; i < compCount; ++i) {
                JpegComponent& comp = components[i];
                comp.id = bytes[p + 6 + i * 3];
                comp.hSamp = bytes[p + 7 + i * 3] >> 4;
                comp.vSamp = bytes[p + 7 + i * 3] & 0x0F;
                comp.qTable = bytes[p + 8 + i * 3];
                if (comp.hSamp == 0 || comp.vSamp == 0 || comp.qTable > 3) return false;
            }
            haveFrame = true;
        } else if (marker == 0xDB) { // DQT
            while (p + 65 <= end) {
                const uint8_t info = bytes[p++];
                const uint8_t tableId = info & 0x0F;
                if ((info >> 4) != 0 || tableId > 3) return false; // 8-bit tables only
                for (int i = 0; i < 64; ++i) quant[tableId].data[kZigzag[i]] = bytes[p + i];
                p += 64;
            }
        } else if (marker == 0xC4) { // DHT
            while (p + 17 <= end) {
                const uint8_t info = bytes[p++];
                const bool isAc = (info >> 4) != 0;
                const uint8_t tableId = info & 0x0F;
                if (tableId > 3) return false;
                JpegHuffmanTable& table = isAc ? huffAc[tableId] : huffDc[tableId];
                int count = 0;
                for (int i = 0; i < 16; ++i) {
                    table.counts[i] = bytes[p++];
                    count += table.counts[i];
                }
                if (count > 256 || p + count > end) return false;
                for (int i = 0; i < count; ++i) table.symbols[i] = bytes[p++];
                jpeg_build_huffman(table);
            }
        } else if (marker == 0xDA) { // SOS
            const uint8_t scanCompCount = bytes[p];
            if (scanCompCount == 0 || p + 1 + scanCompCount * 2 + 3 > end) return false;
            std::vector<uint8_t> scanIds(scanCompCount);
            std::vector<uint8_t> scanTables(scanCompCount);
            for (uint8_t i = 0; i < scanCompCount; ++i) {
                scanIds[i] = bytes[p + 1 + i * 2];
                scanTables[i] = bytes[p + 2 + i * 2];
            }
            haveScan = true;
            // Entropy-coded data starts right after the SOS header.
            JpegBitReader reader{ bytes.data(), bytes.size(), end, 0, 0 };
            // Resolve component → table bindings for this scan.
            std::vector<size_t> scanIndex(scanCompCount);
            for (uint8_t i = 0; i < scanCompCount; ++i) {
                size_t ci = SIZE_MAX;
                for (size_t c = 0; c < components.size(); ++c)
                    if (components[c].id == scanIds[i]) { ci = c; break; }
                if (ci == SIZE_MAX) return false;
                scanIndex[i] = ci;
                components[ci].dcTable = scanTables[i] >> 4;
                components[ci].acTable = scanTables[i] & 0x0F;
            }
            if (!haveFrame || components.empty()) return false;
            uint8_t hMax = 1, vMax = 1;
            for (const JpegComponent& comp : components) {
                hMax = std::max(hMax, comp.hSamp);
                vMax = std::max(vMax, comp.vSamp);
            }
            for (JpegComponent& comp : components) {
                comp.planeW = ((static_cast<uint32_t>(width) * comp.hSamp + hMax * 8 - 1) / (hMax * 8)) * 8;
                comp.planeH = ((static_cast<uint32_t>(height) * comp.vSamp + vMax * 8 - 1) / (vMax * 8)) * 8;
                comp.plane.assign(static_cast<size_t>(comp.planeW) * comp.planeH, 0.0f);
            }
            const uint32_t mcuCols = (static_cast<uint32_t>(width) + hMax * 8 - 1) / (hMax * 8);
            const uint32_t mcuRows = (static_cast<uint32_t>(height) + vMax * 8 - 1) / (vMax * 8);
            float block[64];
            int prevDc[4]{};
            for (uint32_t mcuY = 0; mcuY < mcuRows; ++mcuY) {
                for (uint32_t mcuX = 0; mcuX < mcuCols; ++mcuX) {
                    for (uint8_t s = 0; s < scanCompCount; ++s) {
                        JpegComponent& comp = components[scanIndex[s]];
                        for (uint32_t by = 0; by < comp.vSamp; ++by) {
                            for (uint32_t bx = 0; bx < comp.hSamp; ++bx) {
                                // DC
                                const int dcSymbol = jpeg_decode_huffman(reader, huffDc[comp.dcTable]);
                                if (dcSymbol < 0) return false;
                                int size = dcSymbol & 0x0F;
                                if (size > 11) return false;
                                int diff = 0;
                                if (size > 0) {
                                    int bits = 0;
                                    if (!reader.get_bits(size, &bits)) return false;
                                    diff = jpeg_extend(bits, size);
                                }
                                prevDc[s] += diff;
                                std::fill(block, block + 64, 0.0f);
                                block[0] = static_cast<float>(prevDc[s]) * quant[comp.qTable].data[0];
                                // AC
                                int k = 1;
                                while (k < 64) {
                                    const int symbol = jpeg_decode_huffman(reader, huffAc[comp.acTable]);
                                    if (symbol < 0) return false;
                                    if (symbol == 0) { k = 64; break; } // EOB
                                    const int run = symbol >> 4;
                                    const int acSize = symbol & 0x0F;
                                    if (k + run >= 64 || acSize > 10) return false;
                                    k += run;
                                    if (acSize > 0) {
                                        int bits = 0;
                                        if (!reader.get_bits(acSize, &bits)) return false;
                                        block[k] = static_cast<float>(jpeg_extend(bits, acSize)) *
                                                   quant[comp.qTable].data[k];
                                    }
                                    ++k;
                                }
                                float samples[64];
                                jpeg_idct_8x8(block, samples);
                                const uint32_t dstX = mcuX * static_cast<uint32_t>(comp.hSamp) * 8 + bx * 8;
                                const uint32_t dstY = mcuY * static_cast<uint32_t>(comp.vSamp) * 8 + by * 8;
                                for (int yy = 0; yy < 8; ++yy) {
                                    for (int xx = 0; xx < 8; ++xx) {
                                        const size_t di = static_cast<size_t>(dstY + yy) * comp.planeW + (dstX + xx);
                                        if (di < comp.plane.size()) comp.plane[di] = 128.0f + samples[xx + yy * 8];
                                    }
                                }
                            }
                        }
                    }
                }
            }
            break; // single-scan baseline handled
        } else if (marker == 0xC2 || marker == 0xC1 || marker == 0xC3) {
            return false; // progressive / other SOF variants unsupported
        }
        pos = end;
    }
    if (!haveFrame || !haveScan) return false;

    // ── Assemble RGB (upsample chroma, YCbCr → RGB) ──
    const auto sample = [&](const JpegComponent& comp, uint32_t x, uint32_t y) -> float {
        const uint32_t sx = std::min(x, comp.planeW - 1);
        const uint32_t sy = std::min(y, comp.planeH - 1);
        return comp.plane[static_cast<size_t>(sy) * comp.planeW + sx];
    };
    out.width = width;
    out.height = height;
    out.channels = 3;
    out.bitDepth = 8;
    out.pixels.resize(static_cast<size_t>(width) * height * 3);
    uint8_t hMax = 1, vMax = 1;
    for (const JpegComponent& comp : components) {
        hMax = std::max(hMax, comp.hSamp);
        vMax = std::max(vMax, comp.vSamp);
    }
    const auto ux = [hMax](const JpegComponent& comp, uint32_t x) {
        return x * static_cast<uint32_t>(hMax) / static_cast<uint32_t>(comp.hSamp);
    };
    const auto uy = [vMax](const JpegComponent& comp, uint32_t y) {
        return y * static_cast<uint32_t>(vMax) / static_cast<uint32_t>(comp.vSamp);
    };
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (components.size() == 1) {
                const float v = sample(components[0], ux(components[0], x), uy(components[0], y));
                r = g = b = v;
            } else if (components.size() >= 3) {
                const float yy = sample(components[0], ux(components[0], x), uy(components[0], y));
                const float cb = sample(components[1], ux(components[1], x), uy(components[1], y));
                const float cr = sample(components[2], ux(components[2], x), uy(components[2], y));
                r = yy + 1.402f * (cr - 128.0f);
                g = yy - 0.344136f * (cb - 128.0f) - 0.714136f * (cr - 128.0f);
                b = yy + 1.772f * (cb - 128.0f);
            }
            const auto clampByte = [](float v) -> uint8_t {
                return static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v)));
            };
            const size_t di = (static_cast<size_t>(y) * width + x) * 3;
            out.pixels[di] = clampByte(r);
            out.pixels[di + 1] = clampByte(g);
            out.pixels[di + 2] = clampByte(b);
        }
    }
    return true;
}

} // namespace

bool TextureImporter::supports_extension(std::string_view extension) const {
    const std::string value = lower(std::string(extension));
    return value == ".png" || value == ".tga" || value == ".hdr" || value == ".jpg" || value == ".jpeg" ||
           value == ".exr";
}

ImportResult TextureImporter::import(const ImportRequest& request) const {
    std::ifstream source(request.source, std::ios::binary);
    if (!source) return {false, {}, "Texture source does not exist: " + request.source.string()};
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(source)), {});
    const std::string ext = lower(request.source.extension().string());

    uint32_t width = 0, height = 0, channels = 0;
    uint8_t bitDepth = 0;
    std::vector<uint8_t> payload;

    if (ext == ".png") {
        constexpr std::array<uint8_t, 8> pngSignature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (bytes.size() < 26 || !std::equal(pngSignature.begin(), pngSignature.end(), bytes.begin()) ||
            std::string_view(reinterpret_cast<const char*>(bytes.data() + 12), 4) != "IHDR")
            return {false, {}, "Invalid PNG texture header: " + request.source.string()};
        const auto readBigEndian = [&](size_t offset) {
            return (static_cast<uint32_t>(bytes[offset]) << 24) |
                   (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
                   (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
                   static_cast<uint32_t>(bytes[offset + 3]);
        };
        width = readBigEndian(16);
        height = readBigEndian(20);
        bitDepth = bytes[24];
        const uint8_t colorType = bytes[25];
        channels = colorType == 0 ? 1u : colorType == 2 ? 3u :
                   colorType == 3 ? 1u : colorType == 4 ? 2u :
                   colorType == 6 ? 4u : 0u;
        if (width == 0 || height == 0 || channels == 0 || bitDepth == 0)
            return {false, {}, "Unsupported or invalid PNG texture metadata"};
        payload = std::move(bytes);
    } else if (ext == ".tga") {
        DecodedImage image;
        if (!decode_tga(bytes, image))
            return {false, {}, "Invalid or unsupported TGA texture: " + request.source.string()};
        width = image.width;
        height = image.height;
        channels = image.channels;
        bitDepth = image.bitDepth;
        payload = std::move(image.pixels);
    } else if (ext == ".hdr") {
        DecodedImage image;
        if (!decode_hdr(bytes, image))
            return {false, {}, "Invalid or unsupported Radiance HDR texture: " + request.source.string()};
        width = image.width;
        height = image.height;
        channels = image.channels;
        bitDepth = image.bitDepth;
        payload = std::move(image.pixels);
    } else if (ext == ".jpg" || ext == ".jpeg") {
        DecodedImage image;
        if (!decode_jpeg(bytes, image))
            return {false, {}, "Invalid or unsupported JPEG texture: " + request.source.string()};
        width = image.width;
        height = image.height;
        channels = image.channels;
        bitDepth = image.bitDepth;
        payload = std::move(image.pixels);
    } else if (ext == ".exr") {
        DecodedExr exr;
        std::string exrError;
        if (!decode_exr(bytes, exr, &exrError))
            return {false, {}, "Invalid or unsupported EXR texture: " + request.source.string() +
                    " (" + exrError + ")"};
        width = exr.width;
        height = exr.height;
        channels = 4;
        bitDepth = 32; // RGBA16F (mesmo shape do HDR)
        payload = std::move(exr.rgba16f);
    } else {
        return {false, {}, "Unsupported texture extension: " + ext};
    }

    // Apply import settings (Fase 2): textureQuality box-downscales decoded
    // 8-bit payloads (ladder >=80 full / >=40 half / >=20 quarter / ...) and
    // generateMipmaps appends a full CPU mip chain. srgb is persisted as a
    // header flag the loader uses to pick the SRGB image format.
    const bool decoded8Bit = (ext != ".png") && (bitDepth != 32);
    std::vector<uint8_t> cookedPayload = std::move(payload);
    uint32_t mipCount = 1;
    uint8_t flags = (request.settings.srgb ? 1u : 0u);
    if (decoded8Bit) {
        std::vector<CookedTextureLevel> levels;
        levels.push_back(CookedTextureLevel{ width, height, std::move(cookedPayload) });
        const uint32_t scaleLevels = quality_downscale_levels(request.settings.textureQuality);
        for (uint32_t i = 0; i < scaleLevels && levels.back().width > 1 && levels.back().height > 1; ++i) {
            levels.push_back(box_downscale(levels.back().pixels.data(), levels.back().width,
                                           levels.back().height, channels));
        }
        if (scaleLevels > 0) {   // level 0 is the downscaled base
            levels[0] = std::move(levels.back());
            levels.resize(1);
        }
        if (request.settings.generateMipmaps) {
            while ((levels.back().width > 1 || levels.back().height > 1) && levels.size() < 16) {
                levels.push_back(box_downscale(levels.back().pixels.data(), levels.back().width,
                                               levels.back().height, channels));
            }
        }
        width = levels.front().width;
        height = levels.front().height;
        mipCount = static_cast<uint32_t>(levels.size());
        cookedPayload.clear();
        for (const CookedTextureLevel& level : levels) {
            cookedPayload.insert(cookedPayload.end(), level.pixels.begin(), level.pixels.end());
        }
    }

    std::error_code error;
    std::filesystem::create_directories(request.cookedDirectory, error);
    if (error) return {false, {}, "Cannot create cooked texture directory: " + error.message()};
    AssetMetadata asset;
    asset.id = UUID();
    asset.type = AssetType::Texture;
    asset.sourcePath = std::filesystem::weakly_canonical(request.source, error);
    if (error) asset.sourcePath = request.source.lexically_normal();
    apply_import_settings(asset, request);
    asset.importerVersion = request.importerVersion;
    asset.width = width;
    asset.height = height;
    asset.channels = channels;
    asset.cookedPath = request.cookedDirectory / (std::to_string(asset.contentHash) + ".vctex");

    std::ofstream cooked(asset.cookedPath, std::ios::binary | std::ios::trunc);
    if (!cooked) return {false, {}, "Cannot create cooked texture"};
    cooked.write("VCTEX", 5);
    const uint32_t formatVersion = 3;
    const uint64_t payloadSize = cookedPayload.size();
    cooked.write(reinterpret_cast<const char*>(&formatVersion), sizeof(formatVersion));
    cooked.write(reinterpret_cast<const char*>(&width), sizeof(width));
    cooked.write(reinterpret_cast<const char*>(&height), sizeof(height));
    cooked.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    cooked.write(reinterpret_cast<const char*>(&bitDepth), sizeof(bitDepth));
    cooked.write(reinterpret_cast<const char*>(&mipCount), sizeof(mipCount));
    cooked.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    cooked.write(reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize));
    cooked.write(reinterpret_cast<const char*>(cookedPayload.data()),
                 static_cast<std::streamsize>(cookedPayload.size()));
    if (!cooked) return {false, {}, "Cannot write cooked texture payload"};
    asset.isCooked = true;
    return {true, std::move(asset), {}};
}

bool MeshImporter::supports_extension(std::string_view extension) const {
    const std::string value = lower(std::string(extension));
    return value == ".gltf" || value == ".glb" || value == ".fbx";
}

ImportResult MeshImporter::import(const ImportRequest& request) const {
    std::ifstream source(request.source, std::ios::binary);
    if (!source) return {false, {}, "Mesh source does not exist: " + request.source.string()};
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(source)), {});

    // FBX (binário ou ASCII) via ufbx — biblioteca externa. O resultado entra
    // no mesmo GltfGeometryResult do glTF e cozinha no mesmo .vcmesh v2/v3.
    if (lower(request.source.extension().string()) == ".fbx") {
        std::error_code error;
        std::filesystem::create_directories(request.cookedDirectory, error);
        if (error) return {false, {}, "Cannot create cooked mesh directory: " + error.message()};
        AssetMetadata asset;
        asset.id = UUID();
        asset.type = AssetType::Mesh;
        asset.sourcePath = std::filesystem::weakly_canonical(request.source, error);
        if (error) asset.sourcePath = request.source.lexically_normal();
        apply_import_settings(asset, request);
        asset.importerVersion = request.importerVersion;

        std::string fbxError;
        GltfGeometryResult geometry;
        if (!import_fbx_geometry(bytes, geometry, &fbxError))
            return {false, {}, "Invalid or unsupported FBX mesh: " + request.source.string() + " (" + fbxError + ")"};
        if (request.settings.meshScale != 1.0f) {
            for (GltfMeshPrimitive& primitive : geometry.primitives) {
                for (glm::vec3& position : primitive.positions) position *= request.settings.meshScale;
            }
        }
        asset.primitiveCount = static_cast<uint32_t>(geometry.primitives.size());
        asset.vertexCount = geometry.vertexCount;
        asset.indexCount = geometry.indexCount;
        asset.cookedPath = request.cookedDirectory / (std::to_string(asset.contentHash) + ".vcmesh");
        if (!GltfGeometryParser::write_cooked(asset.cookedPath, geometry, &fbxError))
            return {false, {}, "FBX v2 cook failed: " + fbxError};
        asset.isCooked = true;
        return {true, std::move(asset), {}};
    }

    std::string document;
    if (lower(request.source.extension().string()) == ".glb") {
        if (bytes.size() < 20 || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "glTF")
            return {false, {}, "Invalid GLB header"};
        const auto readLE = [&](size_t offset) {
            return static_cast<uint32_t>(bytes[offset]) |
                   (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                   (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                   (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        };
        const uint32_t version = readLE(4), fileLength = readLE(8), jsonLength = readLE(12);
        if (version != 2 || fileLength != bytes.size() || jsonLength == 0 ||
            20ull + jsonLength > bytes.size() || readLE(16) != 0x4E4F534A)
            return {false, {}, "Invalid GLB v2 JSON chunk"};
        document.assign(reinterpret_cast<const char*>(bytes.data() + 20), jsonLength);
    } else {
        document.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    if (document.find("\"asset\"") == std::string::npos ||
        document.find("\"version\"") == std::string::npos ||
        document.find("\"meshes\"") == std::string::npos)
        return {false, {}, "glTF must contain asset version and meshes"};

    std::vector<uint64_t> accessorCounts;
    const std::regex countPattern(R"("count"\s*:\s*(\d+))");
    for (std::sregex_iterator it(document.begin(), document.end(), countPattern), end; it != end; ++it)
        accessorCounts.push_back(std::stoull((*it)[1].str()));
    const std::regex positionPattern(R"("POSITION"\s*:\s*(\d+))");
    const std::regex indicesPattern(R"("indices"\s*:\s*(\d+))");
    std::smatch positionMatch, indicesMatch;
    if (!std::regex_search(document, positionMatch, positionPattern))
        return {false, {}, "glTF mesh has no POSITION accessor"};
    const size_t positionAccessor = std::stoull(positionMatch[1].str());
    if (positionAccessor >= accessorCounts.size()) return {false, {}, "POSITION accessor is out of range"};
    uint64_t indexCount = 0;
    if (std::regex_search(document, indicesMatch, indicesPattern)) {
        const size_t indexAccessor = std::stoull(indicesMatch[1].str());
        if (indexAccessor >= accessorCounts.size()) return {false, {}, "Index accessor is out of range"};
        indexCount = accessorCounts[indexAccessor];
    }
    const std::regex primitivePattern(R"("attributes"\s*:)");
    const uint32_t primitiveCount = static_cast<uint32_t>(
        std::distance(std::sregex_iterator(document.begin(), document.end(), primitivePattern), std::sregex_iterator()));
    if (primitiveCount == 0) return {false, {}, "glTF contains no mesh primitives"};
    std::vector<std::filesystem::path> externalReferences;
    const std::regex uriPattern(R"uri("uri"\s*:\s*"([^"]+)")uri");
    for (std::sregex_iterator it(document.begin(), document.end(), uriPattern), end; it != end; ++it) {
        const std::string uri = (*it)[1].str();
        if (!uri.starts_with("data:")) externalReferences.push_back(request.source.parent_path() / uri);
    }
    std::sort(externalReferences.begin(), externalReferences.end());
    externalReferences.erase(std::unique(externalReferences.begin(), externalReferences.end()), externalReferences.end());

    std::error_code error;
    std::filesystem::create_directories(request.cookedDirectory, error);
    if (error) return {false, {}, "Cannot create cooked mesh directory: " + error.message()};
    AssetMetadata asset;
    asset.id = UUID();
    asset.type = AssetType::Mesh;
    asset.sourcePath = std::filesystem::weakly_canonical(request.source, error);
    if (error) asset.sourcePath = request.source.lexically_normal();
    apply_import_settings(asset, request);
    asset.importerVersion = request.importerVersion;
    asset.primitiveCount = primitiveCount;
    asset.vertexCount = accessorCounts[positionAccessor];
    asset.indexCount = indexCount;
    asset.cookedPath = request.cookedDirectory / (std::to_string(asset.contentHash) + ".vcmesh");
    // Prefer the binary v2 format (geometry payload) so the runtime never has
    // to re-parse the glTF document; fall back to v1 (raw source payload) when
    // the geometry cannot be extracted at cook time.
    bool cookedV2 = false;
    {
        std::string parseError;
        GltfGeometryResult geometry = GltfGeometryParser::parse(bytes, &parseError);
        if (geometry.success) {
            // Apply the import setting at cook time (Fase 2): meshScale scales
            // every vertex position so the shipped geometry is already in the
            // authored unit scale (loaders never re-scale).
            if (request.settings.meshScale != 1.0f) {
                for (GltfMeshPrimitive& primitive : geometry.primitives) {
                    for (glm::vec3& position : primitive.positions) position *= request.settings.meshScale;
                }
            }
            cookedV2 = GltfGeometryParser::write_cooked(asset.cookedPath, geometry, &parseError);
            if (!cookedV2) {
                std::cerr << "[Cooker] v2 mesh cook failed (" << parseError
                          << "), falling back to v1 for " << request.source.filename().string() << std::endl;
            }
        }
    }
    if (!cookedV2) {
        std::ofstream cooked(asset.cookedPath, std::ios::binary | std::ios::trunc);
        if (!cooked) return {false, {}, "Cannot create cooked mesh"};
        cooked.write("VCMESH", 6);
        const uint32_t formatVersion = 1;
        const uint64_t payloadSize = bytes.size();
        cooked.write(reinterpret_cast<const char*>(&formatVersion), sizeof(formatVersion));
        cooked.write(reinterpret_cast<const char*>(&primitiveCount), sizeof(primitiveCount));
        cooked.write(reinterpret_cast<const char*>(&asset.vertexCount), sizeof(asset.vertexCount));
        cooked.write(reinterpret_cast<const char*>(&asset.indexCount), sizeof(asset.indexCount));
        cooked.write(reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize));
        cooked.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!cooked) return {false, {}, "Cannot write cooked mesh payload"};
    }
    asset.isCooked = true;
    ImportResult result{true, std::move(asset), {}};
    result.externalReferences = std::move(externalReferences);
    return result;
}

bool AnimationClipImporter::supports_extension(std::string_view extension) const {
    const std::string ext = lower(std::string(extension));
    return ext == ".animation" || ext == ".anim";
}

ImportResult AnimationClipImporter::import(const ImportRequest& request) const {
    ImportResult result;
    AnimationClip clip;
    if (!AnimationAssetIO::load_clip(clip, request.source)) {
        result.error = "Invalid animation clip: " + request.source.string();
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(request.cookedDirectory, ec);
    if (ec) { result.error = ec.message(); return result; }
    const auto cooked = request.cookedDirectory / (request.source.stem().string() + ".vcanim");
    if (!AnimationAssetIO::save_clip(clip, cooked)) {
        result.error = "Failed to write cooked animation";
        return result;
    }
    result.asset.id = clip.id;
    result.asset.type = AssetType::Animation;
    result.asset.sourcePath = request.source;
    result.asset.cookedPath = cooked;
    result.asset.importerVersion = request.importerVersion;
    result.asset.durationSeconds = clip.duration;
    result.asset.animationTrackCount = static_cast<uint32_t>(clip.tracks.size());
    for (const auto& track : clip.tracks)
        result.asset.animationKeyframeCount += static_cast<uint32_t>(track.keyFrames.size());
    result.asset.isCooked = true;
    apply_import_settings(result.asset, request);
    result.success = true;
    return result;
}

bool SkeletonImporter::supports_extension(std::string_view extension) const {
    return lower(std::string(extension)) == ".skeleton";
}

ImportResult SkeletonImporter::import(const ImportRequest& request) const {
    ImportResult result;
    SkeletonAsset skeleton;
    if (!AnimationAssetIO::load_skeleton(skeleton, request.source)) {
        result.error = "Invalid skeleton asset: " + request.source.string();
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(request.cookedDirectory, ec);
    if (ec) { result.error = ec.message(); return result; }
    const auto cooked = request.cookedDirectory / (request.source.stem().string() + ".vcskeleton");
    if (!AnimationAssetIO::save_skeleton(skeleton, cooked)) {
        result.error = "Failed to write cooked skeleton";
        return result;
    }
    result.asset.id = skeleton.id;
    result.asset.type = AssetType::Skeleton;
    result.asset.sourcePath = request.source;
    result.asset.cookedPath = cooked;
    result.asset.importerVersion = request.importerVersion;
    result.asset.boneCount = static_cast<uint32_t>(skeleton.bones.size());
    result.asset.isCooked = true;
    apply_import_settings(result.asset, request);
    result.success = true;
    return result;
}

bool AudioImporter::supports_extension(std::string_view extension) const {
    const std::string value = lower(std::string(extension));
    return value == ".wav" || value == ".ogg" || value == ".flac" || value == ".mp3";
}

ImportResult AudioImporter::import(const ImportRequest& request) const {
    std::ifstream source(request.source, std::ios::binary);
    if (!source) return {false, {}, "Audio source does not exist: " + request.source.string()};
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(source)), {});

    const std::string ext = lower(request.source.extension().string());
    // Compressed formats (OGG/FLAC/MP3) are decoded to float PCM via miniaudio
    // so the cooked asset is always uncompressed and runtime-ready.
    if (ext == ".ogg" || ext == ".flac" || ext == ".mp3") {
        std::ofstream rawSource(request.source, std::ios::binary | std::ios::trunc);
        if (!rawSource) return {false, {}, "Cannot re-open audio source"};
        rawSource.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        rawSource.close();
        const auto decoded = Engine::Audio::OggDecoder::decode_file(request.source);
        if (!decoded || !decoded->valid()) {
            return {false, {}, "Decoding compressed audio failed: " + (decoded ? decoded->error : "unknown")};
        }
        std::error_code error;
        std::filesystem::create_directories(request.cookedDirectory, error);
        if (error) return {false, {}, "Cannot create cooked audio directory: " + error.message()};
        AssetMetadata asset;
        asset.id = UUID();
        asset.type = AssetType::Audio;
        asset.sourcePath = std::filesystem::weakly_canonical(request.source, error);
        if (error) asset.sourcePath = request.source.lexically_normal();
        apply_import_settings(asset, request);
        asset.importerVersion = request.importerVersion;
        asset.sampleRate = decoded->sampleRate;
        asset.audioChannels = decoded->channels;
        asset.durationSeconds = static_cast<float>(decoded->samples.size()) /
                                static_cast<float>(std::max(decoded->sampleRate * decoded->channels, 1u));
        asset.cookedPath = request.cookedDirectory / (std::to_string(asset.contentHash) + ".vcaudio");
        std::ofstream cooked(asset.cookedPath, std::ios::binary | std::ios::trunc);
        if (!cooked) return {false, {}, "Cannot create cooked audio"};
        const uint32_t formatVersion = 2; // float PCM payload
        const uint32_t sampleRate = decoded->sampleRate;
        const uint16_t channels = static_cast<uint16_t>(decoded->channels);
        const uint16_t bitsPerSample = 32;
        const uint64_t payloadSize = decoded->samples.size() * sizeof(float);
        cooked.write("VCAUDIO", 7);
        cooked.write(reinterpret_cast<const char*>(&formatVersion), sizeof(formatVersion));
        cooked.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
        cooked.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
        cooked.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
        cooked.write(reinterpret_cast<const char*>(&asset.durationSeconds), sizeof(asset.durationSeconds));
        cooked.write(reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize));
        cooked.write(reinterpret_cast<const char*>(decoded->samples.data()), static_cast<std::streamsize>(payloadSize));
        if (!cooked) return {false, {}, "Cannot write cooked audio payload"};
        asset.isCooked = true;
        return {true, std::move(asset), {}};
    }

    if (bytes.size() < 44 || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "RIFF" ||
        std::string_view(reinterpret_cast<const char*>(bytes.data() + 8), 4) != "WAVE")
        return {false, {}, "Invalid RIFF/WAVE header"};
    const auto read16 = [&](size_t offset) {
        return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
    };
    const auto read32 = [&](size_t offset) {
        return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    };
    if (read32(4) + 8ull > bytes.size()) return {false, {}, "Truncated WAV file"};
    uint16_t format = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0, byteRate = 0, dataSize = 0;
    size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const std::string_view chunk(reinterpret_cast<const char*>(bytes.data() + offset), 4);
        const uint32_t chunkSize = read32(offset + 4);
        const size_t payload = offset + 8;
        if (payload + chunkSize > bytes.size()) return {false, {}, "Truncated WAV chunk"};
        if (chunk == "fmt ") {
            if (chunkSize < 16) return {false, {}, "Invalid WAV fmt chunk"};
            format = read16(payload);
            channels = read16(payload + 2);
            sampleRate = read32(payload + 4);
            byteRate = read32(payload + 8);
            bitsPerSample = read16(payload + 14);
        } else if (chunk == "data") dataSize = chunkSize;
        offset = payload + chunkSize + (chunkSize & 1u);
    }
    if ((format != 1 && format != 3) || channels == 0 || sampleRate == 0 ||
        byteRate == 0 || bitsPerSample == 0 || dataSize == 0)
        return {false, {}, "Unsupported or incomplete WAV metadata"};

    std::error_code error;
    std::filesystem::create_directories(request.cookedDirectory, error);
    if (error) return {false, {}, "Cannot create cooked audio directory: " + error.message()};
    AssetMetadata asset;
    asset.id = UUID();
    asset.type = AssetType::Audio;
    asset.sourcePath = std::filesystem::weakly_canonical(request.source, error);
    if (error) asset.sourcePath = request.source.lexically_normal();
    apply_import_settings(asset, request);
    asset.importerVersion = request.importerVersion;
    asset.sampleRate = sampleRate;
    asset.audioChannels = channels;
    asset.durationSeconds = static_cast<float>(dataSize) / static_cast<float>(byteRate);
    asset.cookedPath = request.cookedDirectory / (std::to_string(asset.contentHash) + ".vcaudio");
    std::ofstream cooked(asset.cookedPath, std::ios::binary | std::ios::trunc);
    if (!cooked) return {false, {}, "Cannot create cooked audio"};
    cooked.write("VCAUDIO", 7);
    const uint32_t formatVersion = 1;
    const uint64_t payloadSize = bytes.size();
    cooked.write(reinterpret_cast<const char*>(&formatVersion), sizeof(formatVersion));
    cooked.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    cooked.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    cooked.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    cooked.write(reinterpret_cast<const char*>(&asset.durationSeconds), sizeof(asset.durationSeconds));
    cooked.write(reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize));
    cooked.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!cooked) return {false, {}, "Cannot write cooked audio payload"};
    asset.isCooked = true;
    return {true, std::move(asset), {}};
}

BinaryCopyImporter::BinaryCopyImporter(
    AssetType type, std::vector<std::string> extensions, std::string cookedExtension)
    : type_(type), extensions_(std::move(extensions)), cookedExtension_(std::move(cookedExtension)) {
    for (std::string& extension : extensions_) extension = lower(extension);
}

bool BinaryCopyImporter::supports_extension(std::string_view extension) const {
    const std::string value = lower(std::string(extension));
    return std::find(extensions_.begin(), extensions_.end(), value) != extensions_.end();
}

ImportResult BinaryCopyImporter::import(const ImportRequest& request) const {
    if (!std::filesystem::is_regular_file(request.source))
        return {false, {}, "Source asset does not exist: " + request.source.string()};
    std::error_code error;
    std::filesystem::create_directories(request.cookedDirectory, error);
    if (error) return {false, {}, "Cannot create cooked asset directory"};

    AssetMetadata asset;
    asset.id = UUID();
    asset.type = type_;
    asset.sourcePath = std::filesystem::weakly_canonical(request.source, error);
    if (error) asset.sourcePath = request.source.lexically_normal();
    apply_import_settings(asset, request);
    asset.importerVersion = request.importerVersion;
    asset.cookedPath = request.cookedDirectory /
        (std::to_string(asset.contentHash) + cookedExtension_);

    if (!std::filesystem::exists(asset.cookedPath)) {
        std::filesystem::copy_file(
            request.source, asset.cookedPath,
            std::filesystem::copy_options::overwrite_existing, error);
        if (error) return {false, {}, "Failed to cook asset: " + error.message()};
    }
    asset.isCooked = true;
    return {true, std::move(asset), {}};
}

bool TextMaterialImporter::supports_extension(std::string_view extension) const {
    return lower(std::string(extension)) == ".material";
}

ImportResult TextMaterialImporter::import(const ImportRequest& request) const {
    if (!std::filesystem::is_regular_file(request.source))
        return {false, {}, "Source material asset does not exist: " + request.source.string()};
    MaterialAsset mat;
    if (!mat.load_from_file(request.source))
        return {false, {}, "Failed to parse material file: " + request.source.string()};

    std::error_code error;
    std::filesystem::create_directories(request.cookedDirectory, error);
    if (error) return {false, {}, "Cannot create cooked material directory"};

    AssetMetadata asset;
    asset.id = mat.id.is_valid() ? mat.id : UUID();
    asset.type = AssetType::Material;
    asset.sourcePath = std::filesystem::weakly_canonical(request.source, error);
    if (error) asset.sourcePath = request.source.lexically_normal();
    apply_import_settings(asset, request);
    asset.importerVersion = request.importerVersion;
    asset.cookedPath = request.cookedDirectory /
        (std::to_string(asset.contentHash) + ".materialbin");

    if (!mat.save_to_file(asset.cookedPath))
        return {false, {}, "Failed to cook material asset to path: " + asset.cookedPath.string()};

    asset.isCooked = true;
    return {true, std::move(asset), {}};
}

void AssetPipeline::add_importer(std::unique_ptr<AssetImporter> importer) {
    if (importer) importers_.push_back(std::move(importer));
}

ImportResult AssetPipeline::import(const ImportRequest& request) {
    const std::string extension = lower(request.source.extension().string());
    for (const auto& importer : importers_) {
        if (!importer->supports_extension(extension)) continue;
        ImportResult result = importer->import(request);
        if (result) {
            std::vector<UUID> dependencies;
            dependencies.reserve(result.externalReferences.size());
            for (const std::filesystem::path& reference : result.externalReferences) {
                if (!std::filesystem::is_regular_file(reference))
                    return {false, {}, "Missing external asset dependency: " + reference.string()};
                ImportResult dependency = import({reference, request.cookedDirectory, request.importerVersion});
                if (!dependency)
                    return {false, {}, "Failed to import dependency " + reference.string() + ": " + dependency.error};
                dependencies.push_back(dependency.asset.id);
            }
            // Reimporting a source path updates the existing asset instead of
            // silently changing every UUID reference to it.
            if (const auto existingId = registry_.find_id(result.asset.sourcePath)) {
                result.asset.id = *existingId;
            }
            if (!registry_.register_asset(result.asset))
                return {false, {}, "Asset registry rejected duplicate path or invalid metadata"};
            if (!registry_.set_dependencies(result.asset.id, std::move(dependencies)))
                return {false, {}, "Asset registry rejected imported dependencies"};
        }
        return result;
    }
    return {false, {}, "No importer supports extension: " + extension};
}

void AssetHotReloadService::watch_registered_assets() {
    writeTimes_.clear();
    for (const AssetMetadata& asset : registry_.snapshot()) {
        std::error_code error;
        const auto time = std::filesystem::last_write_time(asset.sourcePath, error);
        if (!error) writeTimes_[AssetRegistry::normalized_key(asset.sourcePath)] = time;
    }
}

std::vector<AssetMetadata> AssetHotReloadService::poll() {
    std::vector<AssetMetadata> reloaded;
    std::vector<UUID> dirtyAssets;
    for (const AssetMetadata& asset : registry_.snapshot()) {
        std::error_code error;
        const auto currentTime = std::filesystem::last_write_time(asset.sourcePath, error);
        if (error) continue;
        const std::string key = AssetRegistry::normalized_key(asset.sourcePath);
        auto known = writeTimes_.find(key);
        if (known == writeTimes_.end()) {
            writeTimes_[key] = currentTime;
            continue;
        }
        if (currentTime <= known->second) continue;
        dirtyAssets.push_back(asset.id);
        ImportResult result = pipeline_.import({asset.sourcePath, cookedDirectory_, asset.importerVersion, asset.importSettings, "generic"});
        if (!result) continue;
        known->second = currentTime;
        reloaded.push_back(result.asset);
        if (callback_) callback_(result.asset);
    }
    if (!dirtyAssets.empty()) {
        for (const UUID dirty : dirtyAssets) {
            for (const UUID referencer : registry_.referencers_of(dirty)) {
                const auto metadata = registry_.find(referencer);
                if (!metadata) continue;
                ImportResult rebuilt = pipeline_.import({metadata->sourcePath, cookedDirectory_, metadata->importerVersion, metadata->importSettings, "generic"});
                if (rebuilt) reloaded.push_back(rebuilt.asset);
            }
        }
    }
    return reloaded;
}

static std::string make_ddc_key(const DerivedDataKey& key) {
    return AssetRegistry::normalized_key(key.source) + "|" +
           std::to_string(key.sourceHash) + "|" + std::to_string(key.settingsHash) + "|" +
           std::to_string(key.importerVersion) + "|" + key.platform;
}

bool DerivedDataCache::load(const std::filesystem::path& path) {
    std::shared_lock lock(mutex_);
    std::ifstream input(path);
    if (!input) return false;
    std::string magic;
    uint32_t version = 0;
    if (!(input >> magic >> version) || magic != "VulkanEngine.DDC" || version != 1) return false;
    std::unordered_map<std::string, DerivedDataEntry> loaded;
    std::string source, cooked, platform;
    uint64_t sourceHash = 0, settingsHash = 0, contentHash = 0;
    uint32_t importerVersion = 0;
    while (input >> std::quoted(source) >> std::quoted(cooked) >> sourceHash >> settingsHash >> contentHash >> importerVersion >> std::quoted(platform)) {
        DerivedDataEntry entry;
        entry.cookedPath = cooked;
        entry.sourceHash = sourceHash;
        entry.settingsHash = settingsHash;
        entry.contentHash = contentHash;
        entry.importerVersion = importerVersion;
        entry.platform = platform;
        loaded.emplace(source, std::move(entry));
    }
    if (!input.eof()) return false;
    lock.unlock();
    std::unique_lock write(mutex_);
    entries_ = std::move(loaded);
    return true;
}

bool DerivedDataCache::save(const std::filesystem::path& path) const {
    std::shared_lock lock(mutex_);
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    output << "VulkanEngine.DDC 1\n";
    for (const auto& [k, entry] : entries_) {
        output << std::quoted(k) << ' ' << std::quoted(entry.cookedPath.generic_string()) << ' '
               << entry.sourceHash << ' ' << entry.settingsHash << ' ' << entry.contentHash << ' '
               << entry.importerVersion << ' ' << std::quoted(entry.platform) << '\n';
    }
    return output.good();
}

void DerivedDataCache::clear() {
    std::unique_lock lock(mutex_);
    entries_.clear();
}

std::optional<DerivedDataEntry> DerivedDataCache::find(const DerivedDataKey& key) const {
    std::shared_lock lock(mutex_);
    auto it = entries_.find(make_ddc_key(key));
    if (it == entries_.end()) {
        ++stats_.misses;
        return std::nullopt;
    }
    ++stats_.hits;
    return it->second;
}

void DerivedDataCache::store(DerivedDataKey key, DerivedDataEntry entry) {
    std::unique_lock lock(mutex_);
    entries_[make_ddc_key(key)] = std::move(entry);
}

bool DerivedDataCache::invalidate_source(const std::filesystem::path& source) {
    std::unique_lock lock(mutex_);
    const auto normalized = AssetRegistry::normalized_key(source);
    const std::string prefix = normalized + "|";
    size_t erased = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            it = entries_.erase(it);
            ++erased;
        } else {
            ++it;
        }
    }
    if (erased == 0) return false;
    stats_.invalidations += erased;
    return true;
}

std::vector<DerivedDataEntry> DerivedDataCache::entries() const {
    std::shared_lock lock(mutex_);
    std::vector<DerivedDataEntry> result;
    result.reserve(entries_.size());
    for (const auto& [k, entry] : entries_) result.push_back(entry);
    return result;
}

DerivedDataStats DerivedDataCache::stats() const {
    std::shared_lock lock(mutex_);
    return stats_;
}

bool DerivedDataCache::trim(size_t maxEntries) {
    std::unique_lock lock(mutex_);
    if (entries_.size() <= maxEntries) return true;
    size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end() && entries_.size() > maxEntries;) {
        it = entries_.erase(it);
        ++removed;
    }
    stats_.evictions += removed;
    return true;
}

AssetPackager::Result AssetPackager::package(const AssetRegistry& registry,
                                              const std::vector<UUID>& roots,
                                              const std::filesystem::path& outputDirectory) {
    if (roots.empty() || outputDirectory.empty()) return {false, {}, {}, "Package roots and output are required"};
    for (UUID root : roots)
        if (!registry.find(root)) return {false, {}, {}, "Package root is not registered: " + root.to_string()};

    const std::vector<UUID> unused = registry.unused_assets(roots);
    const std::unordered_set<UUID> unusedSet(unused.begin(), unused.end());
    std::vector<AssetMetadata> included;
    for (const AssetMetadata& asset : registry.snapshot()) {
        if (unusedSet.contains(asset.id)) continue;
        if (!asset.isCooked || asset.cookedPath.empty() || !std::filesystem::is_regular_file(asset.cookedPath))
            return {false, {}, {}, "Reachable asset is not cooked: " + asset.id.to_string()};
        included.push_back(asset);
    }
    std::sort(included.begin(), included.end(), [](const AssetMetadata& a, const AssetMetadata& b) {
        return a.id < b.id;
    });

    const std::filesystem::path staging = outputDirectory.string() + ".staging";
    std::error_code error;
    std::filesystem::remove_all(staging, error);
    std::filesystem::create_directories(staging / "Content", error);
    if (error) return {false, {}, {}, "Cannot create package staging directory: " + error.message()};
    std::ofstream manifest(staging / "AssetManifest.txt", std::ios::trunc);
    if (!manifest) return {false, {}, {}, "Cannot create package manifest"};
    manifest << "VulkanEngine.AssetPackage 1\n";

    Result result;
    for (const AssetMetadata& asset : included) {
        const std::filesystem::path relative = std::filesystem::path("Content") /
            asset.id.to_string() / asset.cookedPath.filename();
        std::filesystem::create_directories((staging / relative).parent_path(), error);
        if (error) return {false, {}, {}, "Cannot create packaged asset directory: " + error.message()};
        std::filesystem::copy_file(asset.cookedPath, staging / relative,
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) return {false, {}, {}, "Cannot copy cooked asset: " + error.message()};
        manifest << std::quoted(asset.id.to_string()) << ' ' << static_cast<int>(asset.type) << ' '
                 << std::quoted(relative.generic_string()) << ' ' << asset.contentHash << '\n';
        result.assets.push_back(asset.id);
    }
    manifest.close();
    if (!manifest) return {false, {}, {}, "Cannot finalize package manifest"};
    std::filesystem::remove_all(outputDirectory, error);
    error.clear();
    std::filesystem::rename(staging, outputDirectory, error);
    if (error) return {false, {}, {}, "Cannot publish package: " + error.message()};
    result.success = true;
    result.manifestPath = outputDirectory / "AssetManifest.txt";
    return result;
}

std::vector<AssetMetadata> AssetBrowserModel::query(
    std::string_view search, std::optional<AssetType> type) const {
    const std::string needle = lower(std::string(search));
    std::vector<AssetMetadata> result;
    for (const AssetMetadata& asset : registry_.snapshot()) {
        if (type && asset.type != *type) continue;
        const std::string searchable = lower(asset.sourcePath.generic_string());
        if (!needle.empty() && searchable.find(needle) == std::string::npos) continue;
        result.push_back(asset);
    }
    std::sort(result.begin(), result.end(), [](const AssetMetadata& a, const AssetMetadata& b) {
        return AssetRegistry::normalized_key(a.sourcePath) < AssetRegistry::normalized_key(b.sourcePath);
    });
    return result;
}

AssetFileOperationResult AssetBrowserModel::move_asset(
    UUID id, const std::filesystem::path& destination) {
    const auto found = registry_.find(id);
    if (!found) return {false, {}, "Asset UUID is not registered"};
    if (destination.empty()) return {false, {}, "Asset destination is empty"};
    if (std::filesystem::exists(destination))
        return {false, {}, "Asset destination already exists: " + destination.string()};

    std::error_code error;
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error) return {false, {}, "Cannot create destination directory: " + error.message()};
    }
    std::filesystem::rename(found->sourcePath, destination, error);
    if (error) return {false, {}, "Cannot move asset: " + error.message()};

    AssetMetadata moved = *found;
    moved.sourcePath = std::filesystem::weakly_canonical(destination, error);
    if (error) moved.sourcePath = destination.lexically_normal();
    if (!registry_.register_asset(moved)) {
        std::error_code rollbackError;
        std::filesystem::rename(destination, found->sourcePath, rollbackError);
        return {false, {}, "Registry rejected moved asset metadata"};
    }
    return {true, std::move(moved), {}};
}

AssetFileOperationResult AssetBrowserModel::duplicate_asset(
    UUID id, const std::filesystem::path& destination) {
    const auto found = registry_.find(id);
    if (!found) return {false, {}, "Asset UUID is not registered"};
    if (destination.empty()) return {false, {}, "Asset destination is empty"};
    if (std::filesystem::exists(destination))
        return {false, {}, "Asset destination already exists: " + destination.string()};

    std::error_code error;
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error) return {false, {}, "Cannot create destination directory: " + error.message()};
    }
    std::filesystem::copy_file(found->sourcePath, destination, error);
    if (error) return {false, {}, "Cannot duplicate asset: " + error.message()};

    AssetMetadata copy = *found;
    copy.id = UUID();
    copy.sourcePath = std::filesystem::weakly_canonical(destination, error);
    if (error) copy.sourcePath = destination.lexically_normal();
    copy.cookedPath.clear();
    copy.isCooked = false;
    if (!registry_.register_asset(copy)) {
        std::filesystem::remove(destination, error);
        return {false, {}, "Registry rejected duplicated asset metadata"};
    }
    return {true, std::move(copy), {}};
}

AssetFileOperationResult AssetBrowserModel::delete_asset(UUID id) {
    const auto found = registry_.find(id);
    if (!found) return {false, {}, "Asset UUID is not registered"};
    const std::vector<UUID> referencers = registry_.referencers_of(id);
    if (!referencers.empty())
        return {false, *found, "Asset is referenced by " + std::to_string(referencers.size()) + " asset(s)"};

    const std::filesystem::path source = found->sourcePath;
    const std::filesystem::path cooked = found->cookedPath;
    const std::string suffix = ".delete-" + id.to_string();
    const std::filesystem::path sourceTrash = source.string() + suffix;
    const std::filesystem::path cookedTrash = cooked.empty() ? std::filesystem::path{} :
        std::filesystem::path(cooked.string() + suffix);
    std::error_code error;
    if (std::filesystem::exists(source)) {
        std::filesystem::rename(source, sourceTrash, error);
        if (error) return {false, *found, "Cannot stage source deletion: " + error.message()};
    }
    if (!cooked.empty() && cooked != source && std::filesystem::exists(cooked)) {
        std::filesystem::rename(cooked, cookedTrash, error);
        if (error) {
            std::error_code rollback;
            if (std::filesystem::exists(sourceTrash)) std::filesystem::rename(sourceTrash, source, rollback);
            return {false, *found, "Cannot stage cooked deletion: " + error.message()};
        }
    }
    if (!registry_.remove_asset(id)) {
        std::error_code rollback;
        if (std::filesystem::exists(sourceTrash)) std::filesystem::rename(sourceTrash, source, rollback);
        if (!cookedTrash.empty() && std::filesystem::exists(cookedTrash))
            std::filesystem::rename(cookedTrash, cooked, rollback);
        return {false, *found, "Registry rejected asset deletion"};
    }
    std::filesystem::remove(sourceTrash, error);
    if (!cookedTrash.empty()) std::filesystem::remove(cookedTrash, error);
    return {true, *found, {}};
}

} // namespace Engine
