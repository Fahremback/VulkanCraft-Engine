// ManifoldAdapter.cpp
//
// Manifold CSG boolean operations adapter (Apache 2.0).
// Uses the vendored Manifold library from external/solutions/manifold/.
// Supports Union, Subtract, and Intersect operations on triangle meshes.

#include "engine/physics/ICSGOperation.hpp"

#include "manifold/manifold.h"
#include "manifold/mesh.h"

#include <string>
#include <vector>

namespace engine {
namespace physics {
namespace {

class ManifoldAdapter final : public ICSGOperation {
public:
    const char* name() const override { return "Manifold"; }

    bool operate(const CSGMesh& a, const CSGMesh& b,
                 CSGOp op, CSGMesh& result,
                 std::string& errorOut) override {
        if (a.positions.size() < 9 || a.indices.size() < 3) {
            errorOut = "manifold: mesh A has fewer than 3 vertices";
            return false;
        }
        if (b.positions.size() < 9 || b.indices.size() < 3) {
            errorOut = "manifold: mesh B has fewer than 3 vertices";
            return false;
        }

        // Convert to Manifold MeshGL format.
        manifold::MeshGL meshA = toMeshGL(a);
        manifold::MeshGL meshB = toMeshGL(b);

        // Create Manifold objects.
        manifold::Manifold manA(meshA);
        manifold::Manifold manB(meshB);

        if (manA.Status() != manifold::Manifold::Error::NoError) {
            errorOut = "manifold: mesh A is not a valid manifold";
            return false;
        }
        if (manB.Status() != manifold::Manifold::Error::NoError) {
            errorOut = "manifold: mesh B is not a valid manifold";
            return false;
        }

        // Perform boolean operation.
        manifold::OpType mOp;
        switch (op) {
            case CSGOp::Union:     mOp = manifold::OpType::Add; break;
            case CSGOp::Subtract:  mOp = manifold::OpType::Subtract; break;
            case CSGOp::Intersect: mOp = manifold::OpType::Intersect; break;
        }

        manifold::Manifold manResult = manA.Boolean(manB, mOp);

        if (manResult.Status() != manifold::Manifold::Error::NoError) {
            errorOut = "manifold: boolean operation failed (invalid result)";
            return false;
        }

        // Convert back to our format.
        manifold::MeshGL resultGL = manResult.GetMeshGL();
        result = fromMeshGL(resultGL);
        return true;
    }

private:
    static manifold::MeshGL toMeshGL(const CSGMesh& mesh) {
        manifold::MeshGL mgl;
        mgl.numProp = 3; // xyz only
        mgl.vertProperties.clear();
        mgl.vertProperties.reserve(mesh.positions.size());
        for (float v : mesh.positions) {
            mgl.vertProperties.push_back(v);
        }
        mgl.triVerts.clear();
        mgl.triVerts.reserve(mesh.indices.size());
        for (uint32_t idx : mesh.indices) {
            mgl.triVerts.push_back(idx);
        }
        return mgl;
    }

    static CSGMesh fromMeshGL(const manifold::MeshGL& mgl) {
        CSGMesh mesh;
        mesh.positions.clear();
        mesh.indices.clear();
        if (mgl.numProp < 3) return mesh;

        std::size_t vertCount = mgl.vertProperties.size() / mgl.numProp;
        mesh.positions.reserve(vertCount * 3);
        for (std::size_t i = 0; i < vertCount; ++i) {
            mesh.positions.push_back(mgl.vertProperties[i * mgl.numProp + 0]);
            mesh.positions.push_back(mgl.vertProperties[i * mgl.numProp + 1]);
            mesh.positions.push_back(mgl.vertProperties[i * mgl.numProp + 2]);
        }
        mesh.indices.reserve(mgl.triVerts.size());
        for (uint32_t idx : mgl.triVerts) {
            mesh.indices.push_back(idx);
        }
        return mesh;
    }
};

} // namespace

std::unique_ptr<ICSGOperation> create_csg_operation(
    const std::string& backend, std::string& errorOut) {
    if (backend == "manifold" || backend.empty()) {
        return std::make_unique<ManifoldAdapter>();
    }
    errorOut = "csg: unknown backend '" + backend + "'";
    return nullptr;
}

} // namespace physics
} // namespace engine
