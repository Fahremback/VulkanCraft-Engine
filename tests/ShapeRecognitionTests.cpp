// ShapeRecognitionTests — gate do contrato público de reconhecimento de
// formas (IShapeRecognition). Fecha o item G.shape-ml: a contraparte
// headless/determinística do reconhecimento de forma do catálogo,
// implementada do zero no SDK (RANSAC), sem acoplamento a licença GPL.
//
// Prova: detecção de plano, esfera e caixa em nuvens sintéticas com a fração
// de apoio esperada, determinismo bit-exact, validação all-or-nothing e
// factory JSON.

#include "engine/physics/IShapeRecognition.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

using engine::physics::create_shape_recognition;
using engine::physics::create_shape_recognition_json;
using engine::physics::PrimitiveKind;
using engine::physics::ShapeRecognitionConfig;
using engine::physics::ShapePrimitive;

// Plano z = 0: grade 10x10 + 10 pontos de ruído afastados.
std::vector<glm::vec3> plane_cloud() {
    std::vector<glm::vec3> pts;
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x)
            pts.emplace_back(static_cast<float>(x) * 0.1f - 0.45f,
                             static_cast<float>(y) * 0.1f - 0.45f, 0.0f);
    for (int i = 0; i < 10; ++i) {
        const float a = static_cast<float>(i) * 0.7f;
        pts.emplace_back(std::sin(a) * 3.0f, std::cos(a) * 3.0f,
                         2.0f + 0.3f * static_cast<float>(i));
    }
    return pts;
}

// Esfera unitária: 64 pontos determinísticos (amostragem por ângulos).
std::vector<glm::vec3> sphere_cloud() {
    std::vector<glm::vec3> pts;
    for (int i = 0; i < 64; ++i) {
        const float u = (static_cast<float>(i) + 0.5f) / 64.0f;
        const float v = (static_cast<float>((i * 37) % 64) + 0.5f) / 64.0f;
        const float theta = 2.0f * 3.14159265f * u;
        const float phi = std::acos(1.0f - 2.0f * v);
        pts.emplace_back(std::sin(phi) * std::cos(theta),
                         std::sin(phi) * std::sin(theta), std::cos(phi));
    }
    return pts;
}

// Caixa: 8 cantos + 6 centros de face + 6 meios de aresta (20 pontos).
std::vector<glm::vec3> box_cloud() {
    std::vector<glm::vec3> pts;
    for (int x = -1; x <= 1; x += 2)
        for (int y = -1; y <= 1; y += 2)
            for (int z = -1; z <= 1; z += 2)
                pts.emplace_back(static_cast<float>(x),
                                 static_cast<float>(y),
                                 static_cast<float>(z));
    const float f[2] = { -1.0f, 1.0f };
    for (const float s : f) {
        pts.emplace_back(s, 0.0f, 0.0f);
        pts.emplace_back(0.0f, s, 0.0f);
        pts.emplace_back(0.0f, 0.0f, s);
        pts.emplace_back(s, s, 0.0f);
        pts.emplace_back(s, 0.0f, s);
        pts.emplace_back(0.0f, s, s);
    }
    return pts;
}

void test_plane_detection() {
    ShapeRecognitionConfig config;
    config.minSupport = 8;
    std::string error;
    auto rec = create_shape_recognition(error);
    check(rec->configure(config, error), "configure ok");
    std::vector<ShapePrimitive> out;
    check(rec->recognize(plane_cloud(), out, error) && error.empty(),
          "plane cloud recognized");
    check(!out.empty() && out[0].kind == PrimitiveKind::Plane,
          "first primitive is a plane");
    check(out[0].support >= 100,
          "plane holds the 100 grid points (>= minSupport)");
    // Plano em z = 0: normal ±z.
    check(std::fabs(std::fabs(out[0].normal.z) - 1.0f) < 1e-3f,
          "plane normal is the z axis");
}

void test_sphere_detection() {
    ShapeRecognitionConfig config;
    config.minSupport = 8;
    config.maxIterations = 512;
    std::string error;
    auto rec = create_shape_recognition(error);
    check(rec->configure(config, error), "configure ok");
    std::vector<ShapePrimitive> out;
    check(rec->recognize(sphere_cloud(), out, error) && error.empty(),
          "sphere cloud recognized");
    check(!out.empty() && out[0].kind == PrimitiveKind::Sphere,
          "first primitive is a sphere");
    check(out[0].support >= 60, "sphere holds >= 60 of the 64 points");
    check(std::fabs(out[0].radius - 1.0f) < 0.05f,
          "sphere radius ~ 1");
    check(glm::length(out[0].center) < 0.05f, "sphere center ~ origin");
}

void test_box_detection() {
    ShapeRecognitionConfig config;
    config.minSupport = 20;  // esfera circunscrita dos cantos só tem 8
    config.maxIterations = 256;
    std::string error;
    auto rec = create_shape_recognition(error);
    check(rec->configure(config, error), "configure ok");
    std::vector<ShapePrimitive> out;
    check(rec->recognize(box_cloud(), out, error) && error.empty(),
          "box cloud recognized");
    check(!out.empty() && out[0].kind == PrimitiveKind::Box,
          "first primitive is a box");
    if (!out.empty()) {
        check(out[0].support == 20, "box holds all 20 points");
        check(std::fabs(out[0].extents.x - 1.0f) < 1e-3f &&
                  std::fabs(out[0].extents.y - 1.0f) < 1e-3f &&
                  std::fabs(out[0].extents.z - 1.0f) < 1e-3f,
              "box extents ~ 1");
    }
}

void test_determinism() {
    ShapeRecognitionConfig config;
    config.minSupport = 8;
    config.seed = 5;
    std::string error;
    auto a = create_shape_recognition(error);
    auto b = create_shape_recognition(error);
    check(a->configure(config, error) && b->configure(config, error),
          "configure both ok");
    const auto cloud = plane_cloud();
    std::vector<ShapePrimitive> pa;
    std::vector<ShapePrimitive> pb;
    check(a->recognize(cloud, pa, error) && b->recognize(cloud, pb, error),
          "recognize both ok");
    check(pa.size() == pb.size(), "same primitive count");
    bool same = true;
    for (std::size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
        if (pa[i].kind != pb[i].kind || pa[i].support != pb[i].support ||
            pa[i].inlierIndices != pb[i].inlierIndices)
            same = false;
    }
    check(same, "deterministic: identical primitives and inliers");
}

void test_validation_and_factory() {
    ShapeRecognitionConfig config;
    std::string error;
    auto rec = create_shape_recognition(error);
    check(rec != nullptr, "factory ok");

    ShapeRecognitionConfig bad = config;
    bad.maxIterations = 0;
    check(!bad.valid(error) && !error.empty(), "maxIterations 0 refused");

    bad = config;
    bad.inlierThreshold = 0.0f;
    check(!bad.valid(error) && !error.empty(), "inlierThreshold 0 refused");

    bad = config;
    bad.minSupport = 0;
    check(!bad.valid(error) && !error.empty(), "minSupport 0 refused");

    std::vector<ShapePrimitive> out;
    check(!rec->recognize({}, out, error) && !error.empty(),
          "empty cloud refused");

    error.clear();
    auto json = create_shape_recognition_json(
        R"({"seed":3,"minSupport":12})", error);
    check(json != nullptr && error.empty(), "json factory ok");
    check(json->config().seed == 3 && json->config().minSupport == 12,
          "json config applied");
    auto badJson = create_shape_recognition_json(R"({"maxPoints":0})", error);
    check(badJson == nullptr && !error.empty(), "json invalid refused");
}

}  // namespace

int main() {
    test_plane_detection();
    test_sphere_detection();
    test_box_detection();
    test_determinism();
    test_validation_and_factory();

    if (g_failures == 0) {
        std::cout << "shape_recognition_tests: all checks passed\n";
        return 0;
    }
    std::cout << "shape_recognition_tests: " << g_failures << " failure(s)\n";
    return 1;
}
