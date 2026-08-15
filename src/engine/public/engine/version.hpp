#pragma once

// Public engine version and capability info. This header is part of the
// public SDK: it must stay self-contained (standard library only) so that
// external projects and tools (CLI/MCP) can consume it without engine
// internals or graphics dependencies.

namespace engine {

struct VersionInfo {
    int major{ 1 };
    int minor{ 0 };
    int patch{ 0 };
    const char* codename{ "" };
    const char* abi{ "" };
};

// Current engine version. Bump minor on feature milestones, patch on fixes.
inline VersionInfo engine_version() {
    return VersionInfo{ 1, 2, 0, "voxel sandbox universal", "engine-voxel-1" };
}

// Stable ABI token for the public voxel/registry SDK surface.
inline const char* engine_sdk_abi() {
    return "engine-voxel-1";
}

}  // namespace engine
