// ProbeGrid.cpp — Agente 1 (task_plan A.10), the deterministic DDGI / radiance
// probe-grid pure core behind the public IProbeGrid contract.
//
// Self-contained (std + glm): toroidal 3D window that scrolls with the camera,
// six-axis capture through a sampler seam, EMA history accumulation,
// variance-driven relocation (clamped per frame) and backface classification
// resets — all under a per-frame budget. Bit-exact for the same inputs on every
// machine (no RNG; pure functions of the deterministic iteration order). The
// GPU seam (RTXGI vendor code) is out of scope here, as for the other pure
// cores.

#include "engine/rendering/IProbeGrid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Engine::Rendering {

namespace {

const glm::vec3 kAxes[6] = {
    glm::vec3(1.0f, 0.0f, 0.0f),   // +X
    glm::vec3(-1.0f, 0.0f, 0.0f),  // -X
    glm::vec3(0.0f, 1.0f, 0.0f),   // +Y
    glm::vec3(0.0f, -1.0f, 0.0f),  // -Y
    glm::vec3(0.0f, 0.0f, 1.0f),   // +Z
    glm::vec3(0.0f, 0.0f, -1.0f),  // -Z
};

// Toroidal wrap of a per-axis offset into the window [-half, +half).
int wrapOffset(int offset, int half, int res) {
    int o = ((offset + half) % res + res) % res;
    return o - half;
}

// Maps a world cell to its stable slot index within the window (toroidal).
std::uint32_t slotForCell(const glm::ivec3& cell, int res) {
    const int x = ((cell.x % res) + res) % res;
    const int y = ((cell.y % res) + res) % res;
    const int z = ((cell.z % res) + res) % res;
    return static_cast<std::uint32_t>((z * res + y) * res + x);
}

struct Slot {
    glm::ivec3 cell{ 0 };
    glm::vec3 irradiance{ 0.0f };
    glm::vec3 offset{ 0.0f };
    std::uint32_t age{ 0 };
    std::uint32_t flags{ 0 };  // bit0 fresh, bit1 relocated, bit2 classified
    std::uint32_t resets{ 0 };
    bool fresh{ true };
};

class ProbeGrid final : public IProbeGrid {
public:
    ProbeGrid() : config_(ProbeGridConfig{}) {}

    bool configure(const ProbeGridConfig& config, std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        const bool realloc = config_.resolution != config.resolution ||
                             config_.cellSize != config.cellSize;
        config_ = config;
        if (realloc) {
            const std::uint32_t n = config_.resolution * config_.resolution *
                                    config_.resolution;
            slots_.assign(n, Slot{});
            // Initial window around the origin: slot s holds the cell whose
            // toroidal index is s (window [camCell - half, camCell + half)).
            const int res = static_cast<int>(config_.resolution);
            const int half = res / 2;
            for (std::uint32_t s = 0; s < n; ++s) {
                const int x = static_cast<int>(s % res) - half;
                const int y = static_cast<int>((s / res) % res) - half;
                const int z = static_cast<int>(s / (res * res)) - half;
                slots_[s].cell = glm::ivec3(x, y, z);
            }
            cursor_ = 0;
        }
        return true;
    }

    const ProbeGridConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        ProbeGridConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"resolution\": " << config_.resolution
          << ", \"cellSize\": " << config_.cellSize
          << ", \"probesPerFrame\": " << config_.probesPerFrame
          << ", \"historyWeight\": " << config_.historyWeight
          << ", \"maxRelocationStep\": " << config_.maxRelocationStep
          << ", \"relocationEnabled\": "
          << (config_.relocationEnabled ? "true" : "false")
          << ", \"classificationEnabled\": "
          << (config_.classificationEnabled ? "true" : "false")
          << ", \"backfaceThreshold\": " << config_.backfaceThreshold
          << ", \"seed\": " << config_.seed << " }";
        return o.str();
    }

    std::uint32_t update(const glm::vec3& cameraPosition,
                         const ProbeCaptureSampler& sampler,
                         std::uint32_t budgetOverride,
                         std::string* errorOut) override {
        if (!sampler) {
            if (errorOut) *errorOut = "ProbeGrid: sampler required";
            return 0;
        }
        const int res = static_cast<int>(config_.resolution);
        const int half = res / 2;
        const glm::ivec3 camCell = glm::ivec3(
            glm::floor(cameraPosition / config_.cellSize));

        relocationCount_ = 0;
        classificationCount_ = 0;

        // --- scroll: recycle slots whose cell left the window ---
        // The window is [camCell - half, camCell + half) per axis (res cells);
        // the far edge (off == +half) leaves and wraps to the entering edge
        // (off == -half).
        for (Slot& s : slots_) {
            const glm::ivec3 off = s.cell - camCell;
            if (off.x >= half || off.x < -half || off.y >= half || off.y < -half ||
                off.z >= half || off.z < -half) {
                s.cell = camCell + glm::ivec3(wrapOffset(off.x, half, res),
                                              wrapOffset(off.y, half, res),
                                              wrapOffset(off.z, half, res));
                s.irradiance = glm::vec3(0.0f);
                s.offset = glm::vec3(0.0f);
                s.age = 0;
                s.flags = 1u;  // bit0: fresh (pending first update)
                s.resets = 0;
                s.fresh = true;
            }
        }

        // --- budgeted round-robin updates ---
        const std::uint32_t total = static_cast<std::uint32_t>(slots_.size());
        const std::uint32_t budget =
            (budgetOverride > 0) ? budgetOverride : config_.probesPerFrame;
        std::uint32_t updated = 0;
        for (std::uint32_t i = 0; i < budget && updated < total; ++i) {
            Slot& s = slots_[cursor_];
            updateSlot(s, sampler);
            ++updated;
            cursor_ = (cursor_ + 1) % total;
        }
        return updated;
    }

    std::uint32_t probe_count() const noexcept override {
        return static_cast<std::uint32_t>(slots_.size());
    }

    bool probe(std::uint32_t slot, ProbeGridProbe& out) const override {
        if (slot >= slots_.size()) return false;
        const Slot& s = slots_[slot];
        out.irradiance = s.irradiance;
        out.position = cellCenter(s.cell) + s.offset;
        out.offset = s.offset;
        out.cell = s.cell;
        out.age = s.age;
        out.flags = s.flags;
        out.resets = s.resets;
        out.slot = slot;
        return true;
    }

    std::uint32_t relocation_count() const noexcept override {
        return relocationCount_;
    }
    std::uint32_t classification_count() const noexcept override {
        return classificationCount_;
    }

private:
    glm::vec3 cellCenter(const glm::ivec3& c) const {
        return (glm::vec3(c) + 0.5f) * config_.cellSize;
    }

    void updateSlot(Slot& s, const ProbeCaptureSampler& sampler) {
        const glm::vec3 pos = cellCenter(s.cell) + s.offset;
        glm::vec3 sum(0.0f);
        glm::vec3 maxDir(0.0f, 1.0f, 0.0f);
        float maxL = -1.0f;
        int backfaces[3] = {0, 0, 0};  // per axis pair (X, Y, Z)
        bool freeAxis[3] = {false, false, false};

        for (int a = 0; a < 6; ++a) {
            const ProbeCaptureSample smp = sampler(pos, kAxes[a]);
            sum += smp.radiance;
            if (smp.backface) {
                backfaces[a / 2] += 1;
            } else {
                freeAxis[a / 2] = true;
            }
            const float l = 0.2126f * smp.radiance.r + 0.7152f * smp.radiance.g +
                            0.0722f * smp.radiance.b;
            if (l > maxL) {
                maxL = l;
                maxDir = kAxes[a];
            }
        }

        const int backfaceCount = backfaces[0] + backfaces[1] + backfaces[2];
        const glm::vec3 avg = sum / 6.0f;
        const float avgL = 0.2126f * avg.r + 0.7152f * avg.g + 0.0722f * avg.b;

        // --- classification: probe inside geometry -> reset + push to exit ---
        if (config_.classificationEnabled &&
            backfaceCount >= static_cast<int>(config_.backfaceThreshold)) {
            // Least-occluded axis (ties: X, then Y, then Z).
            int axis = 0;
            int minB = backfaces[0];
            for (int a = 1; a < 3; ++a) {
                if (backfaces[a] < minB) {
                    minB = backfaces[a];
                    axis = a;
                }
            }
            const glm::vec3 axisDir =
                (axis == 0) ? glm::vec3(1.0f, 0.0f, 0.0f)
                            : (axis == 1) ? glm::vec3(0.0f, 1.0f, 0.0f)
                                          : glm::vec3(0.0f, 0.0f, 1.0f);
            // Step toward the free direction of that axis (+axis as tie-break).
            glm::vec3 step = axisDir;
            if (freeAxis[axis] && backfaces[axis] == 1) {
                // Exactly one direction of the axis is free: move toward it.
                const int freeIdx = (axis == 0) ? 0 : (axis == 1) ? 2 : 4;
                const glm::vec3 freeDir = kAxes[freeIdx];
                if (glm::dot(axisDir, freeDir) < 0.0f) step = -axisDir;
            }
            s.irradiance = glm::vec3(0.0f);
            s.age = 0;
            s.flags |= 4u;  // bit2: classified
            s.resets += 1;
            s.offset = glm::clamp(s.offset + step * (config_.cellSize * 0.5f),
                                  -glm::vec3(config_.cellSize * 0.5f),
                                  glm::vec3(config_.cellSize * 0.5f));
            ++classificationCount_;
            s.fresh = false;
            s.flags &= ~1u;
            return;
        }

        // --- history accumulation (EMA) + relocation ---
        if (s.fresh || s.age == 0) {
            s.irradiance = avg;
        } else {
            s.irradiance = glm::mix(s.irradiance, avg, config_.historyWeight);
        }
        ++s.age;

        if (config_.relocationEnabled) {
            const float dominance = (maxL > 0.0f)
                                        ? (maxL - avgL) / (maxL + 1e-6f)
                                        : 0.0f;
            if (dominance > 0.0f) {
                const float step =
                    config_.maxRelocationStep * config_.cellSize * dominance;
                const glm::vec3 newOffset = glm::clamp(
                    s.offset + maxDir * step,
                    -glm::vec3(config_.cellSize * 0.5f),
                    glm::vec3(config_.cellSize * 0.5f));
                if (glm::length(newOffset - s.offset) > 1e-6f) {
                    s.offset = newOffset;
                    s.flags |= 2u;  // bit1: relocated (position moved this frame)
                    ++relocationCount_;
                }
            }
        }
        s.fresh = false;
        s.flags &= ~1u;
    }

    static bool parseJson(const std::string& text, ProbeGridConfig& out,
                          std::string& errorOut) {
        struct Pair {
            std::string key;
            std::string value;
        };
        std::vector<Pair> pairs;
        {
            std::size_t i = 0;
            auto skipWs = [&]() {
                while (i < text.size() &&
                       (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                        text[i] == '\r')) {
                    ++i;
                }
            };
            skipWs();
            if (i >= text.size() || text[i] != '{') {
                errorOut = "ProbeGrid config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "ProbeGrid config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "ProbeGrid config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "ProbeGrid config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "ProbeGrid config: unterminated string";
                        return false;
                    }
                    ++i;
                } else {
                    while (i < text.size() && text[i] != ',' && text[i] != '}') {
                        value.push_back(text[i++]);
                    }
                }
                pairs.push_back({key, value});
                skipWs();
                if (i < text.size() && text[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < text.size() && text[i] == '}') {
                    ++i;
                    break;
                }
                errorOut = "ProbeGrid config: expected ',' or '}'";
                return false;
            }
        }

        ProbeGridConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "ProbeGrid config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "resolution") {
                parsed.resolution = static_cast<std::uint32_t>(std::stoul(p.value));
            } else if (p.key == "cellSize") {
                parsed.cellSize = std::stof(p.value);
            } else if (p.key == "probesPerFrame") {
                parsed.probesPerFrame = static_cast<std::uint32_t>(std::stoul(p.value));
            } else if (p.key == "historyWeight") {
                parsed.historyWeight = std::stof(p.value);
            } else if (p.key == "maxRelocationStep") {
                parsed.maxRelocationStep = std::stof(p.value);
            } else if (p.key == "relocationEnabled") {
                parsed.relocationEnabled = (p.value == "true");
            } else if (p.key == "classificationEnabled") {
                parsed.classificationEnabled = (p.value == "true");
            } else if (p.key == "backfaceThreshold") {
                parsed.backfaceThreshold = static_cast<std::uint32_t>(std::stoul(p.value));
            } else if (p.key == "seed") {
                parsed.seed = static_cast<std::uint32_t>(std::stoul(p.value));
            } else {
                errorOut = "ProbeGrid config: unknown key '" + p.key + "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "ProbeGrid config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "ProbeGrid config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    ProbeGridConfig config_{};
    std::vector<Slot> slots_{};
    std::uint32_t cursor_{ 0 };
    std::uint32_t relocationCount_{ 0 };
    std::uint32_t classificationCount_{ 0 };
};

}  // namespace

bool ProbeGridConfig::valid(std::string& errorOut) const {
    if (resolution < 2 || resolution > 32) {
        errorOut = "resolution must be in [2, 32]";
        return false;
    }
    if (!(cellSize >= 0.5f && cellSize <= 64.0f)) {
        errorOut = "cellSize must be in [0.5, 64]";
        return false;
    }
    if (probesPerFrame < 1) {
        errorOut = "probesPerFrame must be >= 1";
        return false;
    }
    if (!(historyWeight >= 0.01f && historyWeight <= 1.0f)) {
        errorOut = "historyWeight must be in [0.01, 1]";
        return false;
    }
    if (!(maxRelocationStep >= 0.0f && maxRelocationStep <= 1.0f)) {
        errorOut = "maxRelocationStep must be in [0, 1]";
        return false;
    }
    if (backfaceThreshold < 1 || backfaceThreshold > 6) {
        errorOut = "backfaceThreshold must be in [1, 6]";
        return false;
    }
    if (seed == 0) {
        errorOut = "seed must be non-zero";
        return false;
    }
    return true;
}

std::unique_ptr<IProbeGrid> create_probe_grid(std::string& errorOut) {
    auto impl = std::make_unique<ProbeGrid>();
    if (!impl) {
        errorOut = "ProbeGrid: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IProbeGrid> create_probe_grid_json(const std::string& jsonText,
                                                   std::string& errorOut) {
    auto impl = std::make_unique<ProbeGrid>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering
