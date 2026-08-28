// VulkanLoader.cpp — Agente 1 (task_plan C.21 volk — NOVO, do zero). Loads the
// platform Vulkan loader and resolves the function pointers the engine calls,
// under one provider, without vendoring volk. It never invokes device
// functions, so the gate runs deterministically on any machine.
#include "engine/rendering/IVulkanLoader.hpp"

#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define VC_USE_WINDOWS 1
#endif

#ifdef VC_USE_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Engine::Rendering {

namespace {

#ifdef VC_USE_WINDOWS
using LoaderHandle = HMODULE;
inline void* libraryOpen() { return LoadLibraryA("vulkan-1.dll"); }
inline void* librarySymbol(LoaderHandle h, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(h, name));
}
inline void libraryClose(LoaderHandle h) { if (h) FreeLibrary(h); }
using VkGetProc = void* (*)(void* instance, const char* name);
#else
using LoaderHandle = void*;
inline void* libraryOpen() { return dlopen("libvulkan.so.1", RTLD_LAZY); }
inline void* librarySymbol(LoaderHandle h, const char* name) {
    return dlsym(h, name);
}
inline void libraryClose(LoaderHandle h) { if (h) dlclose(h); }
#endif

}  // namespace

namespace {

class VulkanLoader final : public IVulkanLoader {
public:
    VulkanLoader() = default;
    ~VulkanLoader() override {
        if (handle_) libraryClose(handle_);
    }
    VulkanLoader(const VulkanLoader&) = delete;
    VulkanLoader& operator=(const VulkanLoader&) = delete;

    bool load(std::string& errorOut) override {
        if (handle_) return true;
        void* h = libraryOpen();
        if (!h) {
            errorOut = "Vulkan loader library (vulkan-1.dll/.so) not found";
            return false;
        }
        handle_ = static_cast<LoaderHandle>(h);
        getInstanceProcAddr_ = reinterpret_cast<VkGetProc>(
            librarySymbol(handle_, "vkGetInstanceProcAddr"));
        getDeviceProcAddr_ = reinterpret_cast<VkGetProc>(
            librarySymbol(handle_, "vkGetDeviceProcAddr"));
        if (!getInstanceProcAddr_) {
            errorOut = "loader missing vkGetInstanceProcAddr";
            libraryClose(handle_);
            handle_ = nullptr;
            return false;
        }
        resolved_["vkGetInstanceProcAddr"] = reinterpret_cast<void*>(getInstanceProcAddr_);
        resolved_["vkGetDeviceProcAddr"] = reinterpret_cast<void*>(getDeviceProcAddr_);
        return true;
    }

    bool loaded() const noexcept override { return handle_ != nullptr; }

    std::size_t loadedCount() const noexcept override { return resolved_.size(); }

    bool resolve(const char* name, void** outFunction) const override {
        if (!outFunction || !name || !handle_) return false;
        auto it = resolved_.find(name);
        if (it != resolved_.end()) { *outFunction = it->second; return it->second != nullptr; }
        // The Vulkan loader exports the entry points directly; a library symbol
        // lookup is the resolved function pointer. Falling back to the
        // instance trampoline covers forwarded/spec-only entry points.
        void* p = librarySymbol(handle_, name);
        if (!p && getInstanceProcAddr_) {
            using Fn = void* (*)(void*, const char*);
            Fn trampoline = reinterpret_cast<Fn>(reinterpret_cast<void*>(getInstanceProcAddr_));
            p = trampoline(nullptr, name);
        }
        resolved_[name] = p;
        *outFunction = p;
        return p != nullptr;
    }

    bool resolveDevice(const char* name, void* device,
                       void** outFunction) const override {
        if (!outFunction || !name || !getDeviceProcAddr_ || !device) return false;
        using Fn = void* (*)(void*, const char*);
        Fn trampoline = reinterpret_cast<Fn>(reinterpret_cast<void*>(getDeviceProcAddr_));
        void* resolved = trampoline(device, name);
        *outFunction = resolved;
        return resolved != nullptr;
    }

    std::string describe() const override {
        std::ostringstream o;
        if (!handle_) return std::string("vulkan-loader: not loaded");
        o << "vulkan-loader: loaded, " << resolved_.size() << " functions";
        return o.str();
    }

private:
    LoaderHandle handle_{ nullptr };
    VkGetProc getInstanceProcAddr_{ nullptr };
    VkGetProc getDeviceProcAddr_{ nullptr };
    mutable std::unordered_map<std::string, void*> resolved_;
};

}  // namespace

std::unique_ptr<IVulkanLoader> create_vulkan_loader(std::string& errorOut) {
    auto impl = std::make_unique<VulkanLoader>();
    if (!impl) { errorOut = "VulkanLoader: allocation failed"; return nullptr; }
    return impl;
}

}  // namespace Engine::Rendering