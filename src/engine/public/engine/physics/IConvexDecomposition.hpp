// IConvexDecomposition.hpp
//
// PUBLIC seam for convex decomposition (CoACD, V-HACD, etc.).
// Consumed by physics/collision cookers that need to split concave meshes
// into convex parts for rigid body simulation.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace physics {

struct ConvexInput {
    std::vector<float> positions;  // xyz per vertex
    std::vector<uint32_t> indices; // triangle indices (3 per tri)
};

struct ConvexPart {
    std::vector<float> vertices;   // xyz per vertex
    std::vector<uint32_t> triangles; // triangle indices
};

struct ConvexResult {
    std::vector<ConvexPart> parts;
};

class IConvexDecomposition {
public:
    virtual ~IConvexDecomposition() = default;

    // Decompose a concave mesh into convex parts.
    // Returns false on failure with errorOut populated.
    virtual bool decompose(const ConvexInput& input,
                           ConvexResult& result,
                           std::string& errorOut) = 0;

    // Name of the backend (e.g. "CoACD", "V-HACD").
    virtual const char* name() const = 0;
};

std::unique_ptr<IConvexDecomposition> create_convex_decomposition(
    const std::string& backend, std::string& errorOut);

} // namespace physics
} // namespace engine
