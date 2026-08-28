// CoACDAdapter.cpp
//
// CoACD (Convex Approximate Convex Decomposition) adapter.
// Uses the vendored CoACD library from external/solutions/coacd/.
// MIT license, self-contained C++ with Bullet convex hull backend.

#include "engine/physics/IConvexDecomposition.hpp"

#include "coacd.h"

#include <algorithm>
#include <cstdint>
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

        // Canonical part ordering: CoACD is multithreaded (OpenMP) and appends
        // finished parts to its output vector under a write lock, so the part
        // ORDER varies with thread scheduling even when the decomposition
        // itself is identical. Consumers (physics, collision, replay and the
        // determinism contract) need a stable result, so order parts by a
        // deterministic key derived from their geometry. Vertices only come
        // from the same mesh, so a lexicographic key on the first vertices is
        // a total order that is stable across runs, machines and thread
        // scheduling. (The vendored sampler was also made deterministic.)
        std::sort(result.parts.begin(), result.parts.end(),
                  [](const ConvexPart& lhs, const ConvexPart& rhs) {
                      const std::size_t n = std::min(lhs.vertices.size(), rhs.vertices.size());
                      for (std::size_t i = 0; i < n; ++i) {
                          if (lhs.vertices[i] != rhs.vertices[i]) {
                              return lhs.vertices[i] < rhs.vertices[i];
                          }
                      }
                      if (lhs.vertices.size() != rhs.vertices.size()) {
                          return lhs.vertices.size() < rhs.vertices.size();
                      }
                      const std::size_t m = std::min(lhs.triangles.size(), rhs.triangles.size());
                      for (std::size_t i = 0; i < m; ++i) {
                          if (lhs.triangles[i] != rhs.triangles[i]) {
                              return lhs.triangles[i] < rhs.triangles[i];
                          }
                      }
                      return lhs.triangles.size() < rhs.triangles.size();
                  });

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
