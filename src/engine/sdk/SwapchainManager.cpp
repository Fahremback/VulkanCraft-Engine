// SwapchainManager.cpp — Swapchain lifecycle state machine
// Pure CPU logic: resize, recreate, frame acquisition, validation.

#include "engine/rendering/ISwapchainManager.hpp"
#include <algorithm>

namespace vc::rendering {

class SwapchainManagerImpl : public ISwapchainManager {
public:
    bool initialize(const SwapchainConfig& config) override {
        if (!config.validateConfig()) return false;
        cfg_ = config;
        info_.state = SwapchainState::Ready;
        info_.width = config.initialWidth;
        info_.height = config.initialHeight;
        info_.imageCount = config.imageCount;
        info_.presentMode = config.presentMode;
        info_.recreateCount = 0;
        info_.frameIndex = 0;
        return true;
    }

    bool resize(uint32_t newWidth, uint32_t newHeight) override {
        if (info_.state == SwapchainState::Uninitialized) return false;
        // Clamp to minimum
        newWidth = std::max(newWidth, cfg_.minWidth);
        newHeight = std::max(newHeight, cfg_.minHeight);
        if (newWidth == info_.width && newHeight == info_.height) return false;
        info_.width = newWidth;
        info_.height = newHeight;
        info_.state = SwapchainState::OutOfDate;
        return true; // recreation needed
    }

    bool recreate() override {
        if (info_.state == SwapchainState::Uninitialized) return false;
        if (info_.state == SwapchainState::SurfaceLost) return false;
        info_.state = SwapchainState::Ready;
        info_.recreateCount++;
        info_.frameIndex = 0;
        return true;
    }

    int acquireFrame() override {
        if (info_.state != SwapchainState::Ready) return -1;
        int idx = info_.frameIndex;
        info_.frameIndex = (info_.frameIndex + 1) % info_.imageCount;
        return idx;
    }

    void presentFrame() override {
        // State machine: present succeeds if Ready
        // In real Vulkan, this would call vkQueuePresent
    }

    SwapchainInfo getInfo() const override { return info_; }

    SwapchainConfig getConfig() const override { return cfg_; }

    bool isSurfaceLost() const override {
        return info_.state == SwapchainState::SurfaceLost;
    }

    void destroy() override {
        info_.state = SwapchainState::Uninitialized;
        info_.width = 0;
        info_.height = 0;
        info_.imageCount = 0;
        info_.recreateCount = 0;
        info_.frameIndex = 0;
    }

private:
    SwapchainConfig cfg_;
    SwapchainInfo info_;
};

std::unique_ptr<ISwapchainManager> create_swapchain_manager(std::string& errorOut) {
    return std::make_unique<SwapchainManagerImpl>();
}

} // namespace vc::rendering
