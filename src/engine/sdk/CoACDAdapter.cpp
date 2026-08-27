// CoACDAdapter.cpp
//
// CoACD (Convex Approximate Convex Decomposition) adapter.
// Uses the vendored CoACD library from external/solutions/coacd/.
// MIT license, self-contained C++ with Bullet convex hull backend.

#include "engine/physics/IConvexDecomposition.hpp"

#include "coacd.h"

#include <sstream>
#include <string>
#include <vector>

namespace engine {
namespace physics {
namespace {

class CoACDAdapter final : public IConvexDecomposition {
public:
    const char* name() const override { return "CoACD"; }

    bool decompose(const ConvexInput& input,
                   ConvexResult& result,
                   std::string& errorOut) override {
        if (input.positions.size() < 9) {
            errorOut = "coacd: input mesh has fewer than 3 vertices";
            return false;
        }
        if (input.indices.size() < 3 || (input.indices.size() % 3) != 0) {
            errorOut = "coacd: index count must be a multiple of 3";
            return false;
        }

        // Convert to CoACD mesh format.
        coacd::Mesh mesh;
        std::size_t vertCount = input.positions.size() / 3;
        mesh.vertices.reserve(vertCount);
        for (std::size_t i = 0; i < vertCount; ++i) {
            mesh.vertices.push_back({
                static_cast<double>(input.positions[i * 3 + 0]),
                static_cast<double>(input.positions[i * 3 + 1]),
                static_cast<double>(input.positions[i * 3 + 2])
            });
        }

        std::size_t triCount = input.indices.size() / 3;
        mesh.indices.reserve(triCount);
        for (std::size_t i = 0; i < triCount; ++i) {
            mesh.indices.push_back({
                static_cast<int>(input.indices[i * 3 + 0]),
                static_cast<int>(input.indices[i * 3 + 1]),
                static_cast<int>(input.indices[i * 3 + 2])
            });
        }

        // Run CoACD decomposition with default parameters.
        coacd::set_log_level("warning");
        std::vector<coacd::Mesh> parts;
        try {
            parts = coacd::CoACD(mesh);
        } catch (const std::exception& e) {
            errorOut = "coacd: exception during decomposition: " + std::string(e.what());
            return false;
        }

        // Convert output.
        result.parts.clear();
        result.parts.reserve(parts.size());
        for (const auto& part : parts) {
            ConvexPart cp;
            cp.vertices.reserve(part.vertices.size() * 3);
            for (const auto& v : part.vertices) {
                cp.vertices.push_back(static_cast<float>(v[0]));
                cp.vertices.push_back(static_cast<float>(v[1]));
                cp.vertices.push_back(static_cast<float>(v[2]));
            }
            cp.triangles.reserve(part.indices.size() * 3);
            for (const auto& t : part.indices) {
                cp.triangles.push_back(static_cast<uint32_t>(t[0]));
                cp.triangles.push_back(static_cast<uint32_t>(t[1]));
                cp.triangles.push_back(static_cast<uint32_t>(t[2]));
            }
            result.parts.push_back(std::move(cp));
        }

        return true;
    }
};

} // namespace

std::unique_ptr<IConvexDecomposition> create_convex_decomposition(
    const std::string& backend, std::string& errorOut) {
    if (backend == "coacd" || backend.empty()) {
        return std::make_unique<CoACDAdapter>();
    }
    errorOut = "coacd: unknown backend '" + backend + "'";
    return nullptr;
}

} // namespace physics
} // namespace engine
