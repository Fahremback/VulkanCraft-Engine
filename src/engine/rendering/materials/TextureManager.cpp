#include "TextureManager.hpp"
#include "engine/rendering/IKtx2Transcoder.hpp"
#include <cmath>
#include <random>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include "engine/core/logging/Log.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// Real KTX2/Basis path (C.4): decode a .ktx2 layer through the public SDK
// transcoder (ktx-software, CPU-side) into the same RGBA32 raster the WIC
// loader produces, so a texture pack can ship .ktx2 layers (albedo/_n/_s)
// and the material pipeline consumes them with no other code change. The
// transcoder is created once; a missing/unreadable/unsized file falls back
// to the PNG/WIC path below (never a silent magenta or a boot-only probe).
static std::vector<uint8_t> load_ktx2_rgba(const fs::path& path, uint32_t width, uint32_t height) {
    if (!fs::exists(path)) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return {};
    std::string error;
    Engine::Rendering::Ktx2Info info;
    static const auto transcoder = Engine::Rendering::create_ktx2_transcoder();
    if (!transcoder) return {};
    if (!transcoder->open(bytes.data(), bytes.size(), info, error)) {
        std::cerr << "[Textures] ktx2 open refused: " << error << std::endl;
        return {};
    }
    // Only adopt native-size layers; anything else falls back to WIC (the
    // texture array is fixed-size and the transcoder reports real dims).
    if (info.width != width || info.height != height || info.faces != 1 || info.levels < 1) return {};
    std::vector<uint8_t> rgba;
    if (!transcoder->transcodeLevel(0, 0, Engine::Rendering::Ktx2Format::Rgba32, rgba, error)) {
        std::cerr << "[Textures] ktx2 transcode refused: " << error << std::endl;
        return {};
    }
    return rgba.size() == static_cast<size_t>(width) * height * 4 ? std::move(rgba) : std::vector<uint8_t>{};
}

static std::vector<uint8_t> load_wic_rgba(const fs::path& path, uint32_t width, uint32_t height,
                                          WICBitmapInterpolationMode interpolation = WICBitmapInterpolationModeFant) {
    if (!fs::exists(path)) return {};
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) return {};
    }
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), width, height, interpolation)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return {};

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return {};
    return pixels;
}

static const char* minecraft_texture_name(TextureIndex index) {
    switch (index) {
    case TextureIndex::GrassTop: return "grass_block_top";
    case TextureIndex::GrassSide: return "grass_block_side";
    case TextureIndex::Dirt: return "dirt";
    case TextureIndex::Stone: return "stone";
    case TextureIndex::Bedrock: return "bedrock";
    case TextureIndex::Sand: return "sand";
    case TextureIndex::WoodSide: return "oak_log";
    case TextureIndex::WoodTop: return "oak_log_top";
    case TextureIndex::Leaves: return nullptr; // foliage uses the generated 2x2 cluster atlas
    case TextureIndex::Planks: return "oak_planks";
    case TextureIndex::Cobblestone: return "cobblestone";
    case TextureIndex::Glass: return "glass";
    case TextureIndex::Bricks: return "bricks";
    case TextureIndex::Water: return nullptr; // animated water keeps its dedicated shader
    case TextureIndex::Lava: return nullptr;
    case TextureIndex::GrassBlade: return nullptr;
    case TextureIndex::CoalOre: return "coal_ore";
    case TextureIndex::IronOre: return "iron_ore";
    case TextureIndex::GoldOre: return "gold_ore";
    case TextureIndex::DiamondOre: return "diamond_ore";
    case TextureIndex::EmeraldOre: return "emerald_ore";
    case TextureIndex::RedstoneOre: return "redstone_ore";
    case TextureIndex::LapisOre: return "lapis_ore";
    case TextureIndex::CopperOre: return "copper_ore";
    case TextureIndex::BirchWoodSide: return "birch_log";
    case TextureIndex::BirchWoodTop: return "birch_log_top";
    case TextureIndex::BirchLeaves: return "birch_leaves";
    case TextureIndex::BirchPlanks: return "birch_planks";
    case TextureIndex::SpruceWoodSide: return "spruce_log";
    case TextureIndex::SpruceWoodTop: return "spruce_log_top";
    case TextureIndex::SpruceLeaves: return "spruce_leaves";
    case TextureIndex::SprucePlanks: return "spruce_planks";
    case TextureIndex::Granite: return "granite";
    case TextureIndex::Diorite: return "diorite";
    case TextureIndex::Andesite: return "andesite";
    case TextureIndex::Deepslate: return "deepslate";
    case TextureIndex::Blackstone: return "blackstone";
    case TextureIndex::Basalt: return "basalt_side";
    case TextureIndex::Netherrack: return "netherrack";
    case TextureIndex::EndStone: return "end_stone";
    case TextureIndex::Obsidian: return "obsidian";
    case TextureIndex::Sandstone: return "sandstone";
    case TextureIndex::Terracotta: return "terracotta";
    case TextureIndex::Glowstone: return "glowstone";
    case TextureIndex::SeaLantern: return "sea_lantern";
    case TextureIndex::MagmaBlock: return "magma";
    case TextureIndex::CraftingTable: return "crafting_table_front";
    case TextureIndex::Furnace: return "furnace_front";
    case TextureIndex::Chest: return "chest_front";
    case TextureIndex::TNT: return "tnt_side";
    case TextureIndex::Bookshelf: return "bookshelf";
    case TextureIndex::Prismarine: return "prismarine";
    case TextureIndex::MossyCobble: return "mossy_cobblestone";
    case TextureIndex::SnowBlock: return "snow";
    default: return nullptr;
    }
}

static fs::path choose_texture_pack() {
    return fs::path(VULKANCRAFT_SOURCE_DIR) / L"assets/textures/minecraft";
}

static void upload_rgba_array(VkDevice device, VmaAllocator allocator, VkQueue queue, VkCommandPool cmdPool,
                              uint32_t size, uint32_t layers, VkFormat format, const std::vector<uint8_t>& pixels,
                              VkImage& image, VmaAllocation& allocation, VkImageView& view) {
    const uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(size))) + 1;
    VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { size, size, 1 };
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo imageAlloc{};
    imageAlloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VK_CHECK(vmaCreateImage(allocator, &imageInfo, &imageAlloc, &image, &allocation, nullptr));

    AllocatedBuffer staging;
    VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = pixels.size();
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo bufferAlloc{};
    bufferAlloc.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &bufferAlloc, &staging.buffer, &staging.allocation, nullptr));
    void* mapped = nullptr;
    VK_CHECK(vmaMapMemory(allocator, staging.allocation, &mapped));
    memcpy(mapped, pixels.data(), pixels.size());
    vmaUnmapMemory(allocator, staging.allocation);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo commandAlloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAlloc.commandPool = cmdPool;
    commandAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAlloc.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device, &commandAlloc, &cmd));
    VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.layerCount = layers;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    std::vector<VkBufferImageCopy> regions(layers);
    const VkDeviceSize layerBytes = static_cast<VkDeviceSize>(size) * size * 4;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        regions[layer].bufferOffset = layerBytes * layer;
        regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[layer].imageSubresource.layerCount = 1;
        regions[layer].imageSubresource.baseArrayLayer = layer;
        regions[layer].imageExtent = { size, size, 1 };
    }
    vkCmdCopyBufferToImage(cmd, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           layers, regions.data());

    int32_t mipSize = static_cast<int32_t>(size);
    for (uint32_t mip = 1; mip < mipLevels; ++mip) {
        VkImageMemoryBarrier sourceBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        sourceBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        sourceBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.image = image;
        sourceBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        sourceBarrier.subresourceRange.baseMipLevel = mip - 1;
        sourceBarrier.subresourceRange.levelCount = 1;
        sourceBarrier.subresourceRange.layerCount = layers;
        sourceBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &sourceBarrier);

        const int32_t nextSize = std::max(1, mipSize / 2);
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mip - 1;
        blit.srcSubresource.layerCount = layers;
        blit.srcOffsets[1] = { mipSize, mipSize, 1 };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = mip;
        blit.dstSubresource.layerCount = layers;
        blit.dstOffsets[1] = { nextSize, nextSize, 1 };
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        sourceBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sourceBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &sourceBarrier);
        mipSize = nextSize;
    }

    VkImageMemoryBarrier finalBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    finalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    finalBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    finalBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finalBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finalBarrier.image = image;
    finalBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    finalBarrier.subresourceRange.baseMipLevel = mipLevels - 1;
    finalBarrier.subresourceRange.levelCount = 1;
    finalBarrier.subresourceRange.layerCount = layers;
    finalBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    finalBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &finalBarrier);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
    vmaDestroyBuffer(allocator, staging.buffer, staging.allocation);

    VkImageViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = layers;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &view));
}

std::vector<uint8_t> TextureManager::generate_texture_layer(TextureIndex index, int width, int height) {
    std::vector<uint8_t> pcm(width * height * 4);

    std::mt19937 rng(static_cast<uint32_t>(index) * 101 + 7);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            float fx = static_cast<float>(x) / width;
            float fy = static_cast<float>(y) / height;

            float noiseVal = dist(rng);

            uint8_t r = 255, g = 255, b = 255, a = 255;

            switch (index) {
            case TextureIndex::GrassTop:
                r = static_cast<uint8_t>(48 + noiseVal * 24);
                g = static_cast<uint8_t>(112 + noiseVal * 38);
                b = static_cast<uint8_t>(35 + noiseVal * 20);
                break;
            case TextureIndex::GrassSide:
                if (fy > 0.78f - std::sin(fx * 18.0f) * 0.055f - noiseVal * 0.035f) {
                    r = static_cast<uint8_t>(48 + noiseVal * 24);
                    g = static_cast<uint8_t>(112 + noiseVal * 38);
                    b = static_cast<uint8_t>(35 + noiseVal * 20);
                } else {
                    if (noiseVal > 0.82f) {
                        r = static_cast<uint8_t>(105 + noiseVal * 28);
                        g = static_cast<uint8_t>(91 + noiseVal * 24);
                        b = static_cast<uint8_t>(73 + noiseVal * 22);
                    } else {
                        r = static_cast<uint8_t>(74 + noiseVal * 28);
                        g = static_cast<uint8_t>(49 + noiseVal * 19);
                        b = static_cast<uint8_t>(29 + noiseVal * 13);
                    }
                }
                break;
            case TextureIndex::Dirt:
                if (noiseVal > 0.80f) {
                    r = static_cast<uint8_t>(140 + noiseVal * 30);
                    g = static_cast<uint8_t>(130 + noiseVal * 30);
                    b = static_cast<uint8_t>(120 + noiseVal * 30);
                } else {
                    r = static_cast<uint8_t>(85 + noiseVal * 30);
                    g = static_cast<uint8_t>(55 + noiseVal * 20);
                    b = static_cast<uint8_t>(30 + noiseVal * 15);
                }
                break;
            case TextureIndex::Stone:
                r = static_cast<uint8_t>(120 + noiseVal * 40);
                g = static_cast<uint8_t>(120 + noiseVal * 40);
                b = static_cast<uint8_t>(125 + noiseVal * 40);
                break;
            case TextureIndex::Bedrock:
                r = static_cast<uint8_t>(30 + noiseVal * 30);
                g = static_cast<uint8_t>(30 + noiseVal * 30);
                b = static_cast<uint8_t>(35 + noiseVal * 30);
                break;
            case TextureIndex::Sand:
                r = static_cast<uint8_t>(220 + noiseVal * 25);
                g = static_cast<uint8_t>(200 + noiseVal * 25);
                b = static_cast<uint8_t>(140 + noiseVal * 20);
                break;
            case TextureIndex::WoodSide:
                if (static_cast<int>(fx * 16) % 4 == 0) {
                    r = 60; g = 40; b = 20;
                } else {
                    r = static_cast<uint8_t>(110 + noiseVal * 20);
                    g = static_cast<uint8_t>(75 + noiseVal * 15);
                    b = static_cast<uint8_t>(40 + noiseVal * 10);
                }
                break;
            case TextureIndex::WoodTop:
                {
                    float distCenter = std::sqrt((fx - 0.5f) * (fx - 0.5f) + (fy - 0.5f) * (fy - 0.5f));
                    if (static_cast<int>(distCenter * 24.0f) % 2 == 0) {
                        r = 140; g = 100; b = 55;
                    } else {
                        r = 110; g = 75; b = 40;
                    }
                }
                break;
            case TextureIndex::Leaves:
                {
                    const int tileX = std::min(1, static_cast<int>(fx * 2.0f));
                    const int tileY = std::min(1, static_cast<int>(fy * 2.0f));
                    const int variant = tileY * 2 + tileX;
                    const float u = fx * 2.0f - static_cast<float>(tileX);
                    const float v = fy * 2.0f - static_cast<float>(tileY);
                    auto hash = [&](int n) {
                        float h = std::fmod(std::sin(static_cast<float>(variant * 97 + n * 37) * 12.9898f) * 43758.5453f, 1.0f);
                        return h < 0.0f ? h + 1.0f : h;
                    };

                    // Transparent pixels retain nearby green RGB: bilinear filtering cannot create black halos.
                    r = static_cast<uint8_t>(35 + variant * 3);
                    g = static_cast<uint8_t>(78 + variant * 5);
                    b = static_cast<uint8_t>(32 + variant * 2);
                    a = 0;

                    for (int leaf = 0; leaf < 18; ++leaf) {
                        const float t = static_cast<float>(leaf) / 17.0f;
                        const float angle = (hash(leaf * 5 + 1) - 0.5f) * (2.4f + t * 1.2f);
                        const float radius = (0.10f + t * 0.43f) * (0.78f + hash(leaf * 5 + 2) * 0.35f);
                        const float cx = 0.5f + std::sin(angle) * radius;
                        const float cy = 0.08f + t * 0.78f + (hash(leaf * 5 + 3) - 0.5f) * 0.08f;

                        const float bx = 0.5f, by = 0.06f;
                        const float vx = cx - bx, vy = cy - by;
                        const float lineT = std::clamp(((u - bx) * vx + (v - by) * vy) / std::max(0.0001f, vx * vx + vy * vy), 0.0f, 1.0f);
                        const float lineDx = u - (bx + vx * lineT);
                        const float lineDy = v - (by + vy * lineT);
                        if (lineDx * lineDx + lineDy * lineDy < 0.000035f && a == 0) {
                            r = 61; g = 49; b = 28; a = 255;
                        }

                        const float orientation = angle * 0.78f + (hash(leaf * 5 + 4) - 0.5f) * 0.7f;
                        const float dirX = std::sin(orientation), dirY = std::cos(orientation);
                        const float dx = u - cx, dy = v - cy;
                        const float along = dx * dirX + dy * dirY;
                        const float across = dx * dirY - dy * dirX;
                        const float length = 0.13f + hash(leaf * 7 + 11) * 0.09f;
                        const float leafWidth = 0.025f + hash(leaf * 7 + 12) * 0.025f;
                        const float ellipse = (along * along) / (length * length * 0.25f) + (across * across) / (leafWidth * leafWidth);
                        const float serration = 1.0f + std::sin(along * 260.0f) * 0.08f;
                        if (ellipse < serration) {
                            const float variation = hash(leaf * 7 + 13);
                            r = static_cast<uint8_t>(31 + variation * 25 + (along / length + 0.5f) * 10);
                            g = static_cast<uint8_t>(82 + variation * 48 + (along / length + 0.5f) * 20);
                            b = static_cast<uint8_t>(30 + variation * 18);
                            a = 255;
                        }
                    }
                }
                break;
            case TextureIndex::Planks:
                if (static_cast<int>(fy * 8) % 2 == 0) {
                    r = 170; g = 120; b = 70;
                } else {
                    r = 190; g = 140; b = 80;
                }
                break;
            case TextureIndex::Cobblestone:
                if (noiseVal > 0.70f) {
                    r = 90; g = 90; b = 95;
                } else {
                    r = static_cast<uint8_t>(110 + noiseVal * 30);
                    g = static_cast<uint8_t>(110 + noiseVal * 30);
                    b = static_cast<uint8_t>(115 + noiseVal * 30);
                }
                break;
            case TextureIndex::Glass:
                if (fx < 0.05f || fx > 0.95f || fy < 0.05f || fy > 0.95f) {
                    r = 220; g = 240; b = 255; a = 200;
                } else {
                    r = 200; g = 230; b = 255; a = 60;
                }
                break;
            case TextureIndex::Bricks:
                if (static_cast<int>(fy * 8) % 2 == 0 || static_cast<int>(fx * 8) % 2 == 0) {
                    r = 190; g = 65; b = 50;
                } else {
                    r = 210; g = 210; b = 215; // Argamassa branca
                }
                break;
            case TextureIndex::Water:
                {
                    const float bands = 0.5f + 0.5f * std::sin(fx * 31.0f + std::sin(fy * 17.0f) * 2.0f);
                    r = static_cast<uint8_t>(18 + bands * 10);
                    g = static_cast<uint8_t>(95 + bands * 18);
                    b = static_cast<uint8_t>(145 + bands * 22);
                    a = 150;
                }
                break;
            case TextureIndex::Lava:
                r = static_cast<uint8_t>(240 + noiseVal * 15);
                g = static_cast<uint8_t>(90 + noiseVal * 30);
                b = static_cast<uint8_t>(10 + noiseVal * 10);
                break;
            case TextureIndex::GrassBlade:
                {
                    const int blade = std::min(15, static_cast<int>(fx * 16.0f));
                    const float bladeRandom = std::fmod(std::sin(static_cast<float>(blade) * 91.731f) * 43758.5453f, 1.0f);
                    const float normalizedRandom = bladeRandom < 0.0f ? bladeRandom + 1.0f : bladeRandom;
                    const float bladeHeight = 0.58f + normalizedRandom * 0.40f;
                    const float t = fy / bladeHeight;
                    const float localX = (fx * 16.0f) - std::floor(fx * 16.0f) - 0.5f;
                    const float lean = (normalizedRandom - 0.5f) * t * 0.34f;
                    const float halfWidth = std::max(0.018f, (1.0f - t) * 0.23f);

                    if (fy <= bladeHeight && std::abs(localX - lean) < halfWidth) {
                        r = static_cast<uint8_t>(34 + normalizedRandom * 24 + fy * 16);
                        g = static_cast<uint8_t>(91 + normalizedRandom * 36 + fy * 40);
                        b = static_cast<uint8_t>(26 + normalizedRandom * 14 + fy * 13);
                        a = 255;
                    } else {
                        r = 0; g = 0; b = 0; a = 0;
                    }
                }
                break;
            }

            pcm[idx + 0] = r;
            pcm[idx + 1] = g;
            pcm[idx + 2] = b;
            pcm[idx + 3] = a;
        }
    }
    return pcm;
}

void TextureManager::init(VkDevice device, VkPhysicalDevice physicalDevice, VmaAllocator allocator, VkQueue queue, uint32_t queueFamily, VkCommandPool cmdPool) {
    (void)physicalDevice;
    (void)queueFamily;
    constexpr uint32_t texSize = 512;
    const uint32_t layerCount = static_cast<uint32_t>(TextureIndex::Count);
    const size_t layerBytes = static_cast<size_t>(texSize) * texSize * 4;
    std::vector<uint8_t> albedoPixels(layerBytes * layerCount);
    std::vector<uint8_t> normalPixels(layerBytes * layerCount);
    std::vector<uint8_t> specularPixels(layerBytes * layerCount);

    const fs::path pack = choose_texture_pack();
    const fs::path blockDir = pack / L"assets/minecraft/textures/block";
    const fs::path realisticGrassDir = fs::path(VULKANCRAFT_SOURCE_DIR) /
        L"assets/textures/grass/assets/minecraft/textures/block";
    activePackName = pack.empty() ? "procedural" : pack.filename().string();
    VC_LOG_INFO("[Textures] Minecraft LabPBR pack: {}", activePackName);

    for (uint32_t layer = 0; layer < layerCount; ++layer) {
        const TextureIndex index = static_cast<TextureIndex>(layer);
        std::vector<uint8_t> albedo = generate_texture_layer(index, texSize, texSize);
        std::vector<uint8_t> normal(layerBytes);
        std::vector<uint8_t> specular(layerBytes);
        for (size_t pixel = 0; pixel < layerBytes; pixel += 4) {
            normal[pixel + 0] = 128; // tangent X
            normal[pixel + 1] = 128; // tangent Y
            normal[pixel + 2] = 255; // ambient occlusion
            normal[pixel + 3] = 0;   // no displacement unless the pack provides it
            specular[pixel + 0] = 0;   // fully rough
            specular[pixel + 1] = 10;  // dielectric F0 ~= 4%
            specular[pixel + 2] = 0;   // no porosity/subsurface
            specular[pixel + 3] = 255; // no emission
        }

        if (const char* textureName = minecraft_texture_name(index); textureName && !pack.empty()) {
            const fs::path base = blockDir / fs::u8path(textureName);
            // C.4: KTX2/Basis is a first-class pack layer format — a .ktx2
            // wins over the .png for the same base name (albedo/_n/_s).
            if (auto loaded = load_ktx2_rgba(base.wstring() + L".ktx2", texSize, texSize); !loaded.empty())
                albedo = std::move(loaded);
            else if (auto loaded = load_wic_rgba(base.wstring() + L".png", texSize, texSize); !loaded.empty())
                albedo = std::move(loaded);
            if (auto loaded = load_ktx2_rgba(base.wstring() + L"_n.ktx2", texSize, texSize); !loaded.empty())
                normal = std::move(loaded);
            else if (auto loaded = load_wic_rgba(base.wstring() + L"_n.png", texSize, texSize); !loaded.empty())
                normal = std::move(loaded);
            if (auto loaded = load_ktx2_rgba(base.wstring() + L"_s.ktx2", texSize, texSize); !loaded.empty())
                specular = std::move(loaded);
            else if (auto loaded = load_wic_rgba(base.wstring() + L"_s.png", texSize, texSize); !loaded.empty())
                specular = std::move(loaded);
        }
        if (index == TextureIndex::GrassBlade && fs::exists(realisticGrassDir)) {
            constexpr uint32_t tileSize = texSize / 2;
            for (uint32_t tile = 0; tile < 4; ++tile) {
                const fs::path texturePath = realisticGrassDir / (L"g_" + std::to_wstring(tile + 1) + L".png");
                auto grass = load_wic_rgba(texturePath, tileSize, tileSize);
                if (grass.empty()) continue;
                const uint32_t targetX = (tile & 1u) * tileSize;
                const uint32_t targetY = (tile >> 1u) * tileSize;
                for (uint32_t row = 0; row < tileSize; ++row) {
                    memcpy(albedo.data() + (static_cast<size_t>(targetY + row) * texSize + targetX) * 4,
                           grass.data() + static_cast<size_t>(row) * tileSize * 4,
                           static_cast<size_t>(tileSize) * 4);
                }
            }
        }
        if (index == TextureIndex::GrassTop && fs::exists(realisticGrassDir / L"top.png")) {
            if (auto grassTop = load_wic_rgba(realisticGrassDir / L"top.png", texSize, texSize); !grassTop.empty()) {
                albedo = std::move(grassTop);
            }
        }
        if (index == TextureIndex::Leaves && !pack.empty()) {
            constexpr uint32_t tileSize = texSize / 2;
            if (auto oakLeaves = load_wic_rgba(blockDir / L"oak_leaves.png", tileSize, tileSize); !oakLeaves.empty()) {
                for (uint32_t tile = 0; tile < 4; ++tile) {
                    const uint32_t targetX = (tile & 1u) * tileSize;
                    const uint32_t targetY = (tile >> 1u) * tileSize;
                    const float radiusX = 0.46f - static_cast<float>(tile & 1u) * 0.025f;
                    const float radiusY = 0.43f + static_cast<float>(tile >> 1u) * 0.025f;
                    for (uint32_t y = 0; y < tileSize; ++y) {
                        for (uint32_t x = 0; x < tileSize; ++x) {
                            const float u = (static_cast<float>(x) + 0.5f) / tileSize;
                            const float v = (static_cast<float>(y) + 0.5f) / tileSize;
                            const float dx = (u - 0.5f) / radiusX;
                            const float dy = (v - 0.5f) / radiusY;
                            const float angle = std::atan2(dy, dx);
                            const float raggedEdge = 1.0f + std::sin(angle * (7.0f + tile) + tile * 1.7f) * 0.075f
                                                          + std::sin(angle * 13.0f - tile) * 0.035f;
                            const uint32_t sourceX = (x * 2u + tile * 37u) % tileSize;
                            const uint32_t sourceY = (y * 2u + tile * 53u) % tileSize;
                            const size_t source = (static_cast<size_t>(sourceY) * tileSize + sourceX) * 4;
                            const size_t target = (static_cast<size_t>(targetY + y) * texSize + targetX + x) * 4;
                            const float brightness = (oakLeaves[source] + oakLeaves[source + 1] + oakLeaves[source + 2]) / (3.0f * 255.0f);
                            const float variation = static_cast<float>(tile) * 0.035f;
                            albedo[target] = static_cast<uint8_t>(std::clamp(24.0f + brightness * 58.0f + variation * 30.0f, 0.0f, 255.0f));
                            albedo[target + 1] = static_cast<uint8_t>(std::clamp(70.0f + brightness * 125.0f + variation * 38.0f, 0.0f, 255.0f));
                            albedo[target + 2] = static_cast<uint8_t>(std::clamp(22.0f + brightness * 48.0f, 0.0f, 255.0f));
                            const bool insideCluster = std::sqrt(dx * dx + dy * dy) <= raggedEdge;
                            albedo[target + 3] = insideCluster ? oakLeaves[source + 3] : 0;
                        }
                    }
                }
            }
        }
        if (index == TextureIndex::PlayerSkin) {
            const fs::path skinsDir = fs::path(VULKANCRAFT_SOURCE_DIR) / L"assets/skins";
            if (fs::exists(skinsDir)) {
                for (const auto& entry : fs::directory_iterator(skinsDir)) {
                    if (!entry.is_regular_file() || entry.path().extension() != L".png") continue;
                    if (auto skin = load_wic_rgba(entry.path(), texSize, texSize, WICBitmapInterpolationModeNearestNeighbor); !skin.empty()) {
                        albedo = std::move(skin);
                        activePackName += " + skin:" + entry.path().stem().string();
                        break;
                    }
                }
            }
        }

        memcpy(albedoPixels.data() + layerBytes * layer, albedo.data(), layerBytes);
        memcpy(normalPixels.data() + layerBytes * layer, normal.data(), layerBytes);
        memcpy(specularPixels.data() + layerBytes * layer, specular.data(), layerBytes);
    }

    upload_rgba_array(device, allocator, queue, cmdPool, texSize, layerCount, VK_FORMAT_R8G8B8A8_SRGB, albedoPixels,
                      textureArrayImage, textureArrayAllocation, textureArrayImageView);
    upload_rgba_array(device, allocator, queue, cmdPool, texSize, layerCount, VK_FORMAT_R8G8B8A8_UNORM, normalPixels,
                      normalArrayImage, normalArrayAllocation, normalArrayImageView);
    upload_rgba_array(device, allocator, queue, cmdPool, texSize, layerCount, VK_FORMAT_R8G8B8A8_UNORM, specularPixels,
                      specularArrayImage, specularArrayAllocation, specularArrayImageView);

    // Sampler (Amostragem Bilinear + Anizotrópica)
    VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 8.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(std::floor(std::log2(texSize)));

    VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &textureSampler));
}

void TextureManager::cleanup(VkDevice device, VmaAllocator allocator) {
    if (descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        descriptorPool = VK_NULL_HANDLE;
        descriptorSet = VK_NULL_HANDLE;
    }
    if (descriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        descriptorLayout = VK_NULL_HANDLE;
    }
    if (textureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, textureSampler, nullptr);
        textureSampler = VK_NULL_HANDLE;
    }
    if (textureArrayImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, textureArrayImageView, nullptr);
    }
    if (textureArrayImage != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator, textureArrayImage, textureArrayAllocation);
    }
    if (normalArrayImageView != VK_NULL_HANDLE) vkDestroyImageView(device, normalArrayImageView, nullptr);
    if (normalArrayImage != VK_NULL_HANDLE) vmaDestroyImage(allocator, normalArrayImage, normalArrayAllocation);
    if (specularArrayImageView != VK_NULL_HANDLE) vkDestroyImageView(device, specularArrayImageView, nullptr);
    if (specularArrayImage != VK_NULL_HANDLE) vmaDestroyImage(allocator, specularArrayImage, specularArrayAllocation);
}
