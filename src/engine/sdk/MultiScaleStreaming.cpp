// MultiScaleStreaming.cpp
//
// The only implementation of engine::procgen::IMultiScaleStreaming (SDK,
// META §19 "streaming multi-escala" / FALTANTES §15 vision). Pure composition
// of the existing public ILodTerrainSampler — no new backend, headless,
// deterministic.
//
// Model: the world function (IVoxelGenerator) is streamed at SEVERAL scales
// around a focus. Each level delegates to ILodTerrainSampler::sample on an
// anchor-aligned origin derived from the focus: origin = floor(focus /
// cellSize) * cellSize. With power-of-two cell sizes the levels share anchors
// (every cellSize-16 anchor is also a cellSize-1 column and a cellSize-256
// anchor is also a cellSize-16 one), so the stack is cross-level coherent by
// construction and the near level is the exact detail surface.

#include "engine/procgen/IMultiScaleStreaming.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace engine {
namespace procgen {

namespace {

std::string diagnostic(const std::string& message) {
    return "multi-scale streaming: " + message;
}

}  // namespace

class MultiScaleStreaming final : public IMultiScaleStreaming {
public:
    MultiScaleStreaming() : sampler_(create_lod_terrain_sampler()) {}

    bool configure(const std::vector<ScaleLevel>& levels,
                   std::string& errorOut) override {
        if (levels.empty()) {
            errorOut = diagnostic("empty scale stack");
            return false;
        }
        for (const ScaleLevel& level : levels) {
            if (level.cellSize <= 0 || level.cellsX <= 0 || level.cellsZ <= 0) {
                errorOut = diagnostic("non-positive cell size / grid extent");
                return false;
            }
        }
        levels_ = levels;
        errorOut.clear();
        return true;
    }

    bool stream(const voxel::IVoxelGenerator& gen, float focusX, float focusZ,
                std::vector<ScaleStream>& out,
                std::string& errorOut) override {
        if (levels_.empty()) {
            errorOut = diagnostic("scale stack not configured");
            return false;
        }
        if (!std::isfinite(focusX) || !std::isfinite(focusZ)) {
            errorOut = diagnostic("non-finite focus");
            return false;
        }
        out.clear();
        out.reserve(levels_.size());
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            const ScaleLevel& level = levels_[i];
            // Anchor-aligned origin: the focus's grid cell in this level.
            const int originX = static_cast<int>(
                std::floor(static_cast<double>(focusX) / level.cellSize)) *
                                level.cellSize;
            const int originZ = static_cast<int>(
                std::floor(static_cast<double>(focusZ) / level.cellSize)) *
                                level.cellSize;
            ScaleStream stream;
            stream.level = static_cast<int>(i);
            stream.cellSize = level.cellSize;
            stream.originX = originX;
            stream.originZ = originZ;
            std::string sampleError;
            if (!sampler_->sample(gen, originX, originZ, level.cellsX,
                                  level.cellsZ, level.cellSize, stream.cells,
                                  sampleError)) {
                errorOut = diagnostic(sampleError);
                return false;
            }
            out.push_back(std::move(stream));
        }
        errorOut.clear();
        return true;
    }

private:
    std::shared_ptr<ILodTerrainSampler> sampler_;
    std::vector<ScaleLevel> levels_;
};

std::shared_ptr<IMultiScaleStreaming> create_multi_scale_streaming() {
    return std::make_shared<MultiScaleStreaming>();
}

}  // namespace procgen
}  // namespace engine
