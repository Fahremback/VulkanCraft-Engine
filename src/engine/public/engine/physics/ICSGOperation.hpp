// ICSGOperation.hpp
//
// PUBLIC seam for CSG boolean operations (union, subtract, intersect).
// Consumed by physics/collision cookers and editor tools that need to
// combine or carve 3D meshes.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace physics {

enum class CSGOp : uint8_t {
    Union,      // A ∪ B
    Subtract,   // A \ B
    Intersect,  // A ∩ B
};

struct CSGMesh {
    std::vector<float> positions;  // xyz per vertex
    std::vector<uint32_t> indices; // triangle indices (3 per tri)
};

class ICSGOperation {
public:
    virtual ~ICSGOperation() = default;

    // Perform a CSG boolean operation on two meshes.
    // Returns false on failure with errorOut populated.
    virtual bool operate(const CSGMesh& a, const CSGMesh& b,
                         CSGOp op, CSGMesh& result,
                         std::string& errorOut) = 0;

    // Name of the backend (e.g. "Manifold", "Cork").
    virtual const char* name() const = 0;
};

std::unique_ptr<ICSGOperation> create_csg_operation(
    const std::string& backend, std::string& errorOut);

} // namespace physics
} // namespace engine
