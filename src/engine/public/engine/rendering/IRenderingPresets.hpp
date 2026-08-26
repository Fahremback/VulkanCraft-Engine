#pragma once

// IRenderingPresets — Agente 1 (task_plan A.16), the PUBLIC quality-preset
// contract. One DATA-DRIVEN dial that resolves a named quality level into the
// concrete budgets of every rendering contract (GI clipmaps, reflections,
// software tracing, surface-cache capture, diffuse multi-bounce), so the
// editor/CLI/MCP and the renderer agree on "Low/Medium/High/Ultra/Cinematic"
// without hardcoding numbers in each consumer.
//
// Self-contained (std + glm). Deterministic: a preset is a pure function of
// the quality level, and `validate` is all-or-nothing (never clamps).

#include "engine/rendering/IDiffuseGlobalIllumination.hpp"
#include "engine/rendering/IGlobalIlluminationProvider.hpp"
#include "engine/rendering/IReflectionProvider.hpp"
#include "engine/rendering/ISoftwareTracer.hpp"
#include "engine/rendering/ISurfaceCacheCapture.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

enum class QualityLevel : std::uint8_t {
    Low = 0,
    Medium,
    High,
    Ultra,
    Cinematic,
    Count
};

// The full set of budgets a quality level drives, one per rendering contract.
struct RenderingPreset {
    GiClipmapConfig gi;             // A.2 radiance cache
    ReflectionConfig reflection;    // A.1 reflection mode budgets
    SoftwareTraceConfig trace;      // A.7 software tracing
    CaptureConfig capture;          // A.4 surface-cache capture
    DiffuseGiConfig diffuseGi;      // A.5 multi-bounce
};

class IRenderingPresets {
public:
    virtual ~IRenderingPresets() = default;

    // Resolves a quality level into its deterministic preset (always succeeds).
    virtual const RenderingPreset& preset(QualityLevel level) const noexcept = 0;

    // Data-driven lookup by name ("low"/"medium"/.../"cinematic", case-
    // insensitive). Returns false (out untouched) for an unknown name.
    virtual bool preset_by_name(const std::string& name,
                                RenderingPreset& out) const = 0;

    // The canonical display name of a level (e.g. "High").
    virtual const char* name(QualityLevel level) const noexcept = 0;

    // Validates a preset across ALL five sub-configs (all-or-nothing; the same
    // ranges each adapter enforces in its own configure()).
    virtual bool validate(const RenderingPreset& preset,
                          std::string& errorOut) const = 0;

    // All level names, in level order.
    virtual std::vector<std::string> level_names() const = 0;
};

// Public factory (always succeeds).
std::unique_ptr<IRenderingPresets> create_rendering_presets(std::string& errorOut);

}  // namespace Engine::Rendering
