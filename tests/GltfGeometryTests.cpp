#include "GltfGeometry.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

bool near(float a, float b) {
    return std::abs(a - b) < 1e-4f;
}

bool near3(glm::vec3 a, glm::vec3 b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

int failures = 0;
#define CHECK(cond, msg)                                             \
    do {                                                             \
        if (!(cond)) {                                               \
            std::cerr << "FAIL line " << __LINE__ << ": " << msg << "\n"; \
            ++failures;                                              \
        }                                                            \
    } while (0)

// Base64 of 36 bytes: 9 floats (3 positions) + 6 uint16 (3 indices).
std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < data.size() ? data[i + 1] : 0;
        const uint32_t c = i + 2 < data.size() ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < data.size() ? alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < data.size() ? alphabet[triple & 0x3F] : '=');
    }
    return out;
}

void append_f32(std::vector<uint8_t>& out, float v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}

void append_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void append_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void test_data_uri_triangle() {
    std::vector<uint8_t> buffer;
    append_f32(buffer, 0.0f); append_f32(buffer, 0.0f); append_f32(buffer, 0.0f);
    append_f32(buffer, 1.0f); append_f32(buffer, 0.0f); append_f32(buffer, 0.0f);
    append_f32(buffer, 0.0f); append_f32(buffer, 1.0f); append_f32(buffer, 0.0f);
    append_u16(buffer, 0); append_u16(buffer, 1); append_u16(buffer, 2);
    const std::string b64 = base64_encode(buffer);

    const std::string json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 48, "uri": "data:application/octet-stream;base64,)" + b64 + R"("}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 6}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
      ],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}]
    })";

    const std::vector<uint8_t> bytes(json.begin(), json.end());
    std::string error;
    const Engine::GltfGeometryResult result = Engine::GltfGeometryParser::parse(bytes, &error);
    CHECK(result.success, "parse failed: " + error);
    if (!result.success) return;
    CHECK(result.primitives.size() == 1, "expected 1 primitive");
    CHECK(result.vertexCount == 3 && result.indexCount == 3, "wrong counts");
    const auto& primitive = result.primitives[0];
    CHECK(primitive.indexed, "expected indexed primitive");
    CHECK(primitive.positions.size() == 3, "expected 3 positions");
    CHECK(near3(primitive.positions[1], glm::vec3(1, 0, 0)), "position 1 wrong");
    CHECK(primitive.indices.size() == 3 && primitive.indices[2] == 2, "indices wrong");
    // No NORMAL accessor -> flat normals computed (unit length).
    CHECK(primitive.normals.size() == 3, "normals should be computed");
    CHECK(near3(primitive.normals[0], glm::vec3(0, 0, 1)), "flat normal wrong");
    CHECK(near(glm::length(primitive.normals[1]), 1.0f), "normal not normalized");
}

void test_glb_bin_chunk() {
    // Positions + explicit normals in the GLB BIN chunk.
    std::vector<uint8_t> bin;
    append_f32(bin, 0.0f); append_f32(bin, 0.0f); append_f32(bin, 0.0f);
    append_f32(bin, 1.0f); append_f32(bin, 0.0f); append_f32(bin, 0.0f);
    append_f32(bin, 0.0f); append_f32(bin, 1.0f); append_f32(bin, 0.0f);
    append_f32(bin, 0.0f); append_f32(bin, 0.0f); append_f32(bin, 1.0f);  // normal 0
    append_f32(bin, 0.0f); append_f32(bin, 0.0f); append_f32(bin, 1.0f);  // normal 1
    append_f32(bin, 0.0f); append_f32(bin, 0.0f); append_f32(bin, 1.0f);  // normal 2
    append_u16(bin, 0); append_u16(bin, 1); append_u16(bin, 2);

    std::string json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 54}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 36},
        {"buffer": 0, "byteOffset": 72, "byteLength": 6}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}
      ],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}]}]
    })";
    while (json.size() % 4) json.push_back(' ');

    std::vector<uint8_t> glb;
    const auto appendU32 = [&](uint32_t v) {
        glb.push_back(static_cast<uint8_t>(v & 0xFF));
        glb.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        glb.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        glb.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    glb.insert(glb.end(), { 'g', 'l', 'T', 'F' });
    appendU32(2);
    appendU32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
    appendU32(static_cast<uint32_t>(json.size()));
    appendU32(0x4E4F534A);
    glb.insert(glb.end(), json.begin(), json.end());
    appendU32(static_cast<uint32_t>(bin.size()));
    appendU32(0x004E4942);
    glb.insert(glb.end(), bin.begin(), bin.end());

    std::string error;
    const Engine::GltfGeometryResult result = Engine::GltfGeometryParser::parse(glb, &error);
    CHECK(result.success, "GLB parse failed: " + error);
    if (!result.success) return;
    CHECK(result.primitives.size() == 1, "expected 1 primitive");
    const auto& primitive = result.primitives[0];
    CHECK(primitive.normals.size() == 3, "explicit normals missing");
    CHECK(near3(primitive.normals[2], glm::vec3(0, 0, 1)), "normal wrong");
    CHECK(primitive.indices.size() == 3, "indices missing");
}

void test_multi_primitive_and_uv() {
    std::vector<uint8_t> buffer;
    // 2 primitives, each a 3-vertex triangle with positions + uvs + 8-bit indices.
    for (int prim = 0; prim < 2; ++prim) {
        const float base = static_cast<float>(prim) * 10.0f;
        append_f32(buffer, base + 0.0f); append_f32(buffer, 0.0f); append_f32(buffer, 0.0f);
        append_f32(buffer, base + 1.0f); append_f32(buffer, 0.0f); append_f32(buffer, 0.0f);
        append_f32(buffer, base + 0.0f); append_f32(buffer, 1.0f); append_f32(buffer, 0.0f);
        append_f32(buffer, 0.0f); append_f32(buffer, 0.0f); // uv0
        append_f32(buffer, 1.0f); append_f32(buffer, 0.0f); // uv1
        append_f32(buffer, 0.0f); append_f32(buffer, 1.0f); // uv2
        buffer.push_back(0); buffer.push_back(1); buffer.push_back(2);
    }
    const std::string b64 = base64_encode(buffer);
    const std::string json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 126, "uri": "data:application/octet-stream;base64,)" + b64 + R"("}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 24},
        {"buffer": 0, "byteOffset": 60, "byteLength": 3},
        {"buffer": 0, "byteOffset": 63, "byteLength": 36},
        {"buffer": 0, "byteOffset": 99, "byteLength": 24},
        {"buffer": 0, "byteOffset": 123, "byteLength": 3}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
        {"bufferView": 2, "componentType": 5121, "count": 3, "type": "SCALAR"},
        {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC2"},
        {"bufferView": 5, "componentType": 5121, "count": 3, "type": "SCALAR"}
      ],
      "meshes": [{"primitives": [
        {"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2},
        {"attributes": {"POSITION": 3, "TEXCOORD_0": 4}, "indices": 5}
      ]}]
    })";
    const std::vector<uint8_t> bytes(json.begin(), json.end());
    std::string error;
    const Engine::GltfGeometryResult result = Engine::GltfGeometryParser::parse(bytes, &error);
    CHECK(result.success, "multi-primitive parse failed: " + error);
    if (!result.success) return;
    CHECK(result.primitives.size() == 2, "expected 2 primitives");
    CHECK(result.vertexCount == 6 && result.indexCount == 6, "wrong totals");
    const auto& second = result.primitives[1];
    CHECK(near(second.positions[0].x, 10.0f), "second primitive offset wrong");
    CHECK(second.uvs.size() == 3 && near(second.uvs[2].y, 1.0f), "uvs wrong");
    CHECK(second.indices[2] == 2, "8-bit indices wrong");
}

void test_invalid_inputs() {
    std::string error;
    // Not JSON.
    const std::vector<uint8_t> garbage = { 'n', 'o', 't', 'j', 's', 'o', 'n' };
    CHECK(!Engine::GltfGeometryParser::parse(garbage, &error).success, "garbage accepted");
    // Valid JSON but no buffers.
    const std::string empty = R"({"asset":{"version":"2.0"}})";
    const std::vector<uint8_t> emptyBytes(empty.begin(), empty.end());
    CHECK(!Engine::GltfGeometryParser::parse(emptyBytes, &error).success, "empty accepted");
    // GLB with bad magic.
    const std::vector<uint8_t> badGlb = { 'b', 'a', 'd', '!', 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(!Engine::GltfGeometryParser::parse(badGlb, &error).success, "bad GLB accepted");
}

void test_vcmesh_wrapper() {
    // Build a real .vcmesh file (as the MeshImporter would) and parse it back.
    std::vector<uint8_t> buffer;
    append_f32(buffer, 0.0f); append_f32(buffer, 0.0f); append_f32(buffer, 0.0f);
    append_f32(buffer, 2.0f); append_f32(buffer, 0.0f); append_f32(buffer, 0.0f);
    append_f32(buffer, 0.0f); append_f32(buffer, 3.0f); append_f32(buffer, 0.0f);
    append_u16(buffer, 0); append_u16(buffer, 1); append_u16(buffer, 2);
    const std::string b64 = base64_encode(buffer);
    const std::string json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": 48, "uri": "data:application/octet-stream;base64,)" + b64 + R"("}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 6}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
      ],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}]
    })";

    const auto tmp = std::filesystem::temp_directory_path() / "gltf_geometry_vcmesh_test.vcmesh";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write("VCMESH", 6);
        const uint32_t version = 1, primitives = 1, verts = 3, indices = 3;
        const uint64_t payloadSize = json.size();
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));
        out.write(reinterpret_cast<const char*>(&primitives), sizeof(primitives));
        out.write(reinterpret_cast<const char*>(&verts), sizeof(verts));
        out.write(reinterpret_cast<const char*>(&indices), sizeof(indices));
        out.write(reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize));
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
    }
    std::string error;
    const Engine::GltfGeometryResult result = Engine::GltfGeometryParser::parse_vcmesh(tmp, &error);
    CHECK(result.success, "vcmesh parse failed: " + error);
    if (result.success) {
        CHECK(result.primitives.size() == 1 && result.primitives[0].positions.size() == 3, "vcmesh geometry wrong");
    }
    // Bad magic must be rejected.
    {
        const auto bad = std::filesystem::temp_directory_path() / "gltf_geometry_bad.vcmesh";
        std::error_code bec;
        std::filesystem::remove(bad, bec);
        {
            std::ofstream out(bad, std::ios::binary | std::ios::trunc);
            out << "NOTVCMESH";
        }
        const Engine::GltfGeometryResult badResult = Engine::GltfGeometryParser::parse_vcmesh(bad, &error);
        CHECK(!badResult.success, "bad vcmesh accepted");
        std::error_code ignoredRemove;
        std::filesystem::remove(bad, ignoredRemove);
    }
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
}

void test_vcmesh_v2_binary() {
    // v2 roundtrip: write_cooked produces a binary geometry payload that
    // parse_vcmesh decodes without any JSON (multi-primitive, uvs, indices).
    Engine::GltfGeometryResult geometry;
    geometry.success = true;
    Engine::GltfMeshPrimitive first;
    first.positions = { {0, 0, 0}, {1, 0, 0}, {0, 1, 0} };
    first.normals = { {0, 0, 1}, {0, 0, 1}, {0, 0, 1} };
    first.uvs = { {0, 0}, {1, 0}, {0, 1} };
    first.indices = { 0, 1, 2 };
    first.indexed = true;
    Engine::GltfMeshPrimitive second;
    second.positions = { {2, 0, 0}, {3, 0, 0}, {2, 1, 0} };
    second.normals = { {0, 0, -1}, {0, 0, -1}, {0, 0, -1} };
    second.indexed = false; // non-indexed primitive
    geometry.primitives = { first, second };
    geometry.vertexCount = 6;
    geometry.indexCount = 3;

    const auto tmp = std::filesystem::temp_directory_path() / "gltf_geometry_v2.vcmesh";
    std::string error;
    CHECK(Engine::GltfGeometryParser::write_cooked(tmp, geometry, &error), "v2 write failed: " + error);
    const Engine::GltfGeometryResult loaded = Engine::GltfGeometryParser::parse_vcmesh(tmp, &error);
    CHECK(loaded.success, "v2 parse failed: " + error);
    if (loaded.success) {
        CHECK(loaded.primitives.size() == 2, "v2 primitive count wrong");
        CHECK(loaded.vertexCount == 6 && loaded.indexCount == 3, "v2 counts wrong");
        const auto& a = loaded.primitives[0];
        CHECK(a.positions.size() == 3 && a.indexed, "v2 indexed primitive wrong");
        CHECK(a.normals.size() == 3 && a.uvs.size() == 3, "v2 normals/uvs lost");
        CHECK(a.indices == first.indices, "v2 indices wrong");
        CHECK(a.positions[1] == glm::vec3(1, 0, 0) && a.uvs[2] == glm::vec2(0, 1), "v2 vertex data wrong");
        const auto& b = loaded.primitives[1];
        CHECK(b.positions.size() == 3 && !b.indexed && b.indices.empty(), "v2 non-indexed primitive wrong");
    }
    // Empty geometry must be rejected by write_cooked.
    {
        Engine::GltfGeometryResult empty;
        empty.success = true;
        CHECK(!Engine::GltfGeometryParser::write_cooked(tmp, empty, &error), "v2 accepted empty geometry");
    }
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
}

void test_skinned_gltf() {
    // A skinned quad: POSITION/NORMAL/TEXCOORD_0/JOINTS_0/WEIGHTS_0 + indices
    // in a data-URI buffer, plus a 2-joint skin with inverse-bind accessor.
    std::vector<uint8_t> buffer;
    const glm::vec3 positions[4] = { {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0} };
    for (const glm::vec3& p : positions) { append_f32(buffer, p.x); append_f32(buffer, p.y); append_f32(buffer, p.z); }
    for (int i = 0; i < 4; ++i) { append_f32(buffer, 0); append_f32(buffer, 0); append_f32(buffer, 1); }
    for (int i = 0; i < 4; ++i) { append_f32(buffer, 0); append_f32(buffer, 0); }
    const uint32_t joints[4][4] = { {0,1,0,0}, {0,1,0,0}, {1,0,0,0}, {1,0,0,0} };
    for (int i = 0; i < 4; ++i) for (int c = 0; c < 4; ++c) append_u32(buffer, joints[i][c]);
    const float weights[4][4] = { {0.75f,0.25f,0,0}, {0.6f,0.4f,0,0}, {0.2f,0.8f,0,0}, {0.1f,0.9f,0,0} };
    for (int i = 0; i < 4; ++i) for (int c = 0; c < 4; ++c) append_f32(buffer, weights[i][c]);
    append_u16(buffer, 0); append_u16(buffer, 1); append_u16(buffer, 2);
    append_u16(buffer, 0); append_u16(buffer, 2); append_u16(buffer, 3);
    // Inverse bind: identity (bone 0) then T(1,0,0) (bone 1).
    for (int m = 0; m < 2; ++m) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                float v = (c == r) ? 1.0f : 0.0f;
                if (m == 1 && c == 3 && r == 0) v = 1.0f;
                append_f32(buffer, v);
            }
        }
    }
    const std::string b64 = base64_encode(buffer);

    const std::string json = R"({
      "asset": {"version": "2.0"},
      "buffers": [{"byteLength": )" + std::to_string(buffer.size()) + R"(, "uri": "data:application/octet-stream;base64,)" + b64 + R"("}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 48},
        {"buffer": 0, "byteOffset": 48, "byteLength": 48},
        {"buffer": 0, "byteOffset": 96, "byteLength": 32},
        {"buffer": 0, "byteOffset": 128, "byteLength": 64},
        {"buffer": 0, "byteOffset": 192, "byteLength": 64},
        {"buffer": 0, "byteOffset": 256, "byteLength": 12},
        {"buffer": 0, "byteOffset": 268, "byteLength": 128}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 3, "componentType": 5125, "count": 4, "type": "VEC4"},
        {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC4"},
        {"bufferView": 5, "componentType": 5123, "count": 6, "type": "SCALAR"},
        {"bufferView": 6, "componentType": 5126, "count": 2, "type": "MAT4"}
      ],
      "nodes": [
        {"name": "Root"},
        {"name": "BoneA", "children": [2]},
        {"name": "BoneB"}
      ],
      "skins": [{"name": "QuadSkin", "joints": [1, 2], "inverseBindMatrices": 6}],
      "meshes": [{"primitives": [{"attributes": {
        "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "JOINTS_0": 3, "WEIGHTS_0": 4
      }, "indices": 5}]}]
    })";

    const std::vector<uint8_t> bytes(json.begin(), json.end());
    std::string error;
    const Engine::GltfGeometryResult result = Engine::GltfGeometryParser::parse(bytes, &error);
    CHECK(result.success, "skinned parse failed: " + error);
    if (!result.success) return;
    CHECK(result.primitives.size() == 1, "expected 1 skinned primitive");
    const auto& primitive = result.primitives[0];
    CHECK(primitive.joints.size() == 4, "JOINTS_0 not extracted");
    CHECK(primitive.weights.size() == 4, "WEIGHTS_0 not extracted");
    if (primitive.joints.size() == 4 && primitive.weights.size() == 4) {
        CHECK(primitive.joints[0] == glm::uvec4(0, 1, 0, 0), "joint 0 wrong");
        CHECK(primitive.joints[3] == glm::uvec4(1, 0, 0, 0), "joint 3 wrong");
        CHECK(near(primitive.weights[1].x, 0.6f) && near(primitive.weights[1].y, 0.4f), "weights 1 wrong");
    }
    CHECK(primitive.indices.size() == 6 && primitive.indexed, "skinned indices wrong");
    CHECK(result.skins.size() == 1, "skin not extracted");
    if (result.skins.size() == 1) {
        const auto& skin = result.skins[0];
        CHECK(skin.name == "QuadSkin", "skin name wrong");
        CHECK(skin.jointNames.size() == 2 && skin.jointNames[0] == "BoneA" && skin.jointNames[1] == "BoneB",
              "joint names wrong");
        CHECK(skin.jointParents.size() == 2 && skin.jointParents[0] == -1 && skin.jointParents[1] == 0,
              "joint parents wrong");
        CHECK(skin.inverseBindMatrices.size() == 2, "inverse bind count wrong");
        CHECK(near(skin.inverseBindMatrices[0][0][0], 1.0f), "identity inverse bind wrong");
        CHECK(near(skin.inverseBindMatrices[1][3][0], 1.0f), "inverse bind translation wrong");
    }

    // Roundtrip through the VCMESH v3 binary payload.
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "vcmesh_skinned_roundtrip.vcmesh";
    CHECK(Engine::GltfGeometryParser::write_cooked(tmp, result, &error), "skinned write_cooked failed");
    const Engine::GltfGeometryResult loaded = Engine::GltfGeometryParser::parse_vcmesh(tmp, &error);
    CHECK(loaded.success, "skinned parse_vcmesh failed: " + error);
    if (loaded.success) {
        CHECK(loaded.primitives.size() == 1, "v3 primitive count wrong");
        const auto& lp = loaded.primitives[0];
        CHECK(lp.joints.size() == 4 && lp.weights.size() == 4, "v3 joints/weights lost");
        if (lp.joints.size() == 4) {
            CHECK(lp.joints[0] == glm::uvec4(0, 1, 0, 0), "v3 joint 0 wrong");
            CHECK(near(lp.weights[2].x, 0.2f) && near(lp.weights[2].y, 0.8f), "v3 weights 2 wrong");
        }
        CHECK(loaded.skins.size() == 1, "v3 skin lost");
        if (loaded.skins.size() == 1) {
            CHECK(loaded.skins[0].jointParents.size() == 2 && loaded.skins[0].jointParents[1] == 0,
                  "v3 parents lost");
            CHECK(near(loaded.skins[0].inverseBindMatrices[1][3][0], 1.0f), "v3 inverse bind lost");
        }
    }
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
}

} // namespace

int main() {
    test_data_uri_triangle();
    test_glb_bin_chunk();
    test_multi_primitive_and_uv();
    test_invalid_inputs();
    test_vcmesh_wrapper();
    test_vcmesh_v2_binary();
    test_skinned_gltf();
    if (failures == 0) {
        std::cout << "GltfGeometryTests: all tests passed\n";
        return 0;
    }
    std::cerr << "GltfGeometryTests: " << failures << " failure(s)\n";
    return 1;
}
