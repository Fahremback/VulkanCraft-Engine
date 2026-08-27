#pragma once
// ISwapchainManager.hpp — Headless swapchain lifecycle: resize, recreate, validation
// State machine for swapchain management without actual Vulkan calls.
// No GPU, no window, no Vulkan device required.

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace vc::rendering {

enum class SwapchainState : uint8_t {
    Uninitialized = 0,
    Ready = 1,
    OutOfDate = 2,
    SurfaceLost = 3,
    Error = 255
};

enum class PresentMode : uint8_t {
    Immediate = 0,
    Fifo = 1,
    FifoRelaxed = 2,
    Mailbox = 3
};

struct SwapchainConfig {
    uint32_t initialWidth = 1920;
    uint32_t initialHeight = 1080;
    uint32_t minWidth = 320;
    uint32_t minHeight = 240;
    uint32_t imageCount = 3;           // triple buffering
    PresentMode presentMode = PresentMode::Fifo;
    bool vsync = true;
    bool validate = true;              // enable validation layers

    bool validateConfig() const {
        return initialWidth >= minWidth && initialHeight >= minHeight
            && imageCount >= 2 && imageCount <= 4;
    }
    std::string toJson() const {
        return "{\"initialWidth\":" + std::to_string(initialWidth)
            + ",\"initialHeight\":" + std::to_string(initialHeight)
            + ",\"minWidth\":" + std::to_string(minWidth)
            + ",\"minHeight\":" + std::to_string(minHeight)
            + ",\"imageCount\":" + std::to_string(imageCount)
            + ",\"presentMode\":" + std::to_string((int)presentMode)
            + ",\"vsync\":" + (vsync ? "true" : "false")
            + ",\"validate\":" + (validate ? "true" : "false") + "}";
    }
    static SwapchainConfig fromJson(const std::string& s) {
        SwapchainConfig c;
        auto fi = [&](const char* k, uint32_t& v) {
            auto p = s.find(std::string("\"") + k + "\":");
            if (p != std::string::npos) v = static_cast<uint32_t>(std::stoul(s.substr(p + std::strlen(k) + 3)));
        };
        fi("initialWidth", c.initialWidth); fi("initialHeight", c.initialHeight);
        fi("minWidth", c.minWidth); fi("minHeight", c.minHeight);
        fi("imageCount", c.imageCount);
        auto p = s.find("\"presentMode\":");
        if (p != std::string::npos) c.presentMode = (PresentMode)std::stoi(s.substr(p + 14));
        p = s.find("\"vsync\":");
        if (p != std::string::npos) c.vsync = s[p + 8] == 't';
        return c;
    }
};

struct SwapchainInfo {
    SwapchainState state = SwapchainState::Uninitialized;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t imageCount = 0;
    PresentMode presentMode = PresentMode::Fifo;
    uint32_t recreateCount = 0;  // how many times recreated
    uint32_t frameIndex = 0;     // current frame (mod imageCount)
};

class ISwapchainManager {
public:
    virtual ~ISwapchainManager() = default;

    // Initialize swapchain
    virtual bool initialize(const SwapchainConfig& config) = 0;

    // Handle resize — returns true if recreation needed
    virtual bool resize(uint32_t newWidth, uint32_t newHeight) = 0;

    // Recreate swapchain (after out-of-date)
    virtual bool recreate() = 0;

    // Acquire next frame — advances frameIndex
    virtual int acquireFrame() = 0;

    // Present current frame
    virtual void presentFrame() = 0;

    // Get current info
    virtual SwapchainInfo getInfo() const = 0;

    // Get config
    virtual SwapchainConfig getConfig() const = 0;

    // Check if surface is lost
    virtual bool isSurfaceLost() const = 0;

    // Destroy
    virtual void destroy() = 0;
};

std::unique_ptr<ISwapchainManager> create_swapchain_manager(std::string& errorOut);

} // namespace vc::rendering
