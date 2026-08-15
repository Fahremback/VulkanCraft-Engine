#pragma once

#include "VulkanTypes.hpp"
#include "Voxel.hpp"
#include <vector>
#include <cstdint>
#include <string>

class TextureManager {
public:
    TextureManager() = default;
    ~TextureManager() = default;

    VkImage textureArrayImage{ VK_NULL_HANDLE };
    VmaAllocation textureArrayAllocation{ VK_NULL_HANDLE };
    VkImageView textureArrayImageView{ VK_NULL_HANDLE };
    VkImage normalArrayImage{ VK_NULL_HANDLE };
    VmaAllocation normalArrayAllocation{ VK_NULL_HANDLE };
    VkImageView normalArrayImageView{ VK_NULL_HANDLE };
    VkImage specularArrayImage{ VK_NULL_HANDLE };
    VmaAllocation specularArrayAllocation{ VK_NULL_HANDLE };
    VkImageView specularArrayImageView{ VK_NULL_HANDLE };
    VkSampler textureSampler{ VK_NULL_HANDLE };

    std::string activePackName{ "procedural" };

    VkDescriptorSetLayout descriptorLayout{ VK_NULL_HANDLE };
    VkDescriptorPool descriptorPool{ VK_NULL_HANDLE };
    VkDescriptorSet descriptorSet{ VK_NULL_HANDLE };

    void init(VkDevice device, VkPhysicalDevice physicalDevice, VmaAllocator allocator, VkQueue queue, uint32_t queueFamily, VkCommandPool cmdPool);
    void cleanup(VkDevice device, VmaAllocator allocator);

private:
    std::vector<uint8_t> generate_texture_layer(TextureIndex index, int width, int height);
};
