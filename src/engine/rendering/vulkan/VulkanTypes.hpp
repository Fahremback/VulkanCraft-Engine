#pragma once

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <format>

constexpr int FRAME_OVERLAP = 2;

struct AllocatedBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VmaAllocation allocation{VK_NULL_HANDLE};
};

#define VK_CHECK(x)                                                                    \
    do {                                                                               \
        VkResult err = x;                                                              \
        if (err) {                                                                     \
            std::cerr << std::format("[Vulkan Error] Code: {}\n", (int)err);           \
            throw std::runtime_error("Vulkan API call failed");                        \
        }                                                                              \
    } while (0)
