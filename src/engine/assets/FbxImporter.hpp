#pragma once

#include "GltfGeometry.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace Engine {

// Imports an FBX file (binary or ASCII) via ufbx (biblioteca externa,
// single-file) into the same GltfGeometryResult the glTF importer produces —
// positions/normals/UVs/indices (+ JOINTS_0/WEIGHTS_0 + skeleton quando o FBX
// tem skin). Assim o .fbx cozinha no mesmo .vcmesh v2/v3 do glTF e o runtime
// não precisa conhecer FBX. Normais ausentes são geradas pelo ufbx; malhas
// poligonais são trianguladas automaticamente.
bool import_fbx_geometry(std::span<const uint8_t> bytes, GltfGeometryResult& out,
                         std::string* error = nullptr);

} // namespace Engine
