#pragma once

// IVulkanLoader — Agente 1 (task_plan C.21 volk — NOVO, criado do zero em
// código nativo). Provides the dynamic Vulkan function-pointer loading the
// project recommended for a single loader provider (findings #232), WITHOUT
// vendoring volk itself. It loads vulkan-1.dll and resolves the function
// pointers that the engine actually calls, under one owner.
//
// SCOPE (headless, deterministic, safe to gate without a GPU/device):
//   load    — load the Vulkan loader library (vulkan-1.dll / libvulkan) and
//             resolve the "global" instance-entrypoint trampoline
//             (vkGetInstanceProcAddr / vkGetDeviceProcAddr);
//   resolve — resolve a named instance-level function via vkGetInstanceProcAddr
//             given an instance, and (optionally) a device-level function via
//             vkGetDeviceProcAddr. Missing/invalid names return nullptr;
//   status  — expose whether the loader library was found and which global
//             trampolines are present.
// This mirrors volk's dynamic loading behavior under one provider the renderer
// can use; it never calls into a device, so the gate runs on any machine.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Engine::Rendering {

class IVulkanLoader {
public:
    virtual ~IVulkanLoader() = default;

    // Loads the shared Vulkan loader for the current platform. Returns true
    // when a loader library was found and the two global trampolines were
    // resolved. Does NOT require a Vulkan device or window.
    virtual bool load(std::string& errorOut) = 0;

    // Whether a loader library was loaded successfully.
    virtual bool loaded() const noexcept = 0;

    // Number of global/loader entry points currently resolved.
    virtual std::size_t loadedCount() const noexcept = 0;

    // Resolve an instance-level function `name` for `instance`. Returns
    // non-null only when both the loader is loaded and the function resolves.
    // The returned value is an opaque identifier of the resolved pointer
    // (implementation may key it by index). Invalid names resolve to null.
    virtual bool resolve(const char* name, void** outFunction) const = 0;

    // Device-level resolution variant (requires a `device` handle). When the
    // device trampoline is unavailable this returns false.
    virtual bool resolveDevice(const char* name, void* device,
                               void** outFunction) const = 0;

    // Human-readable summary for logging/telemetry (never a substitute for the
    // actual functions).
    virtual std::string describe() const = 0;
};

// ---- public factory ----

std::unique_ptr<IVulkanLoader> create_vulkan_loader(std::string& errorOut);

}  // namespace Engine::Rendering