// XrMathTests.cpp — Gate for IXrMath (C.9 openxr — pure quaternion/pose/matrix math)
#include <cstdio>
#include <cmath>
#include <cstring>
#include "engine/rendering/IXrMath.hpp"

static int g_passed = 0, g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); g_failed++; } \
    else { g_passed++; } \
} while(0)

#define CHECK_NEAR(a, b, eps, msg) do { \
    float _diff = std::fabs((a)-(b)); \
    if (_diff > (eps)) { std::printf("  FAIL: %s (got %f, expected %f, diff %f)\n", msg, (float)(a), (float)(b), _diff); g_failed++; } \
    else { g_passed++; } \
} while(0)

#define CHECK_VEC_NEAR(a, b, eps, msg) do { \
    CHECK_NEAR((a).x, (b).x, eps, msg " x"); \
    CHECK_NEAR((a).y, (b).y, eps, msg " y"); \
    CHECK_NEAR((a).z, (b).z, eps, msg " z"); \
} while(0)

#define CHECK_QUAT_NEAR(a, b, eps, msg) do { \
    float _dot = (a).x*(b).x + (a).y*(b).y + (a).z*(b).z + (a).w*(b).w; \
    CHECK_NEAR(std::fabs(_dot), 1.0f, eps, msg " (quaternion equivalence)"); \
} while(0)

using namespace vc::rendering;

int main() {
    std::printf("[xr_math] ALL tests starting\n");

    // 1. Config
    std::printf("[xr_math] test config\n");
    {
        XrConfig c;
        CHECK(c.validate(), "default config valid");
        XrConfig bad; bad.epsilon = -1;
        std::string err;
        auto p = create_xr_math(bad, err);
        CHECK(p == nullptr, "bad epsilon rejected");
    }

    // 2. JSON round-trip
    std::printf("[xr_math] test JSON\n");
    {
        XrConfig c; c.epsilon = 1e-6f;
        std::string json = c.toJson();
        auto c2 = XrConfig::fromJson(json);
        CHECK_NEAR(c2.epsilon, 1e-6f, 1e-7f, "JSON round-trip epsilon");
    }

    // 3. Quaternion identity
    std::printf("[xr_math] test quat identity\n");
    {
        auto q = create_xr_math(XrConfig{}, *(new std::string()));
        XrQuat id = q->quatIdentity();
        CHECK_NEAR(id.w, 1.0f, 1e-6f, "identity w");
        CHECK_NEAR(id.x, 0.0f, 1e-6f, "identity x");
        CHECK_NEAR(id.y, 0.0f, 1e-6f, "identity y");
        CHECK_NEAR(id.z, 0.0f, 1e-6f, "identity z");
    }

    // 4. Quaternion from axis angle (90° around Z)
    std::printf("[xr_math] test quat axis-angle\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrVec3 axis{0,0,1};
        XrQuat q90 = m->quatFromAxisAngle(axis, 3.14159265f / 2.0f);
        // Should rotate (1,0,0) to (0,1,0)
        XrVec3 v{1,0,0};
        XrVec3 result = m->quatRotateVector(q90, v);
        CHECK_NEAR(result.x, 0.0f, 0.01f, "90deg rot x");
        CHECK_NEAR(result.y, 1.0f, 0.01f, "90deg rot y");
        CHECK_NEAR(result.z, 0.0f, 0.01f, "90deg rot z");
    }

    // 5. Quaternion multiply
    std::printf("[xr_math] test quat multiply\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrVec3 z{0,0,1};
        XrQuat q90 = m->quatFromAxisAngle(z, 3.14159265f / 2.0f);
        XrQuat q180 = m->quatMultiply(q90, q90);
        XrVec3 v{1,0,0};
        XrVec3 result = m->quatRotateVector(q180, v);
        CHECK_NEAR(result.x, -1.0f, 0.01f, "180deg rot x");
        CHECK_NEAR(result.y, 0.0f, 0.01f, "180deg rot y");
        CHECK_NEAR(result.z, 0.0f, 0.01f, "180deg rot z");
    }

    // 6. Quaternion invert (q * q^-1 = identity)
    std::printf("[xr_math] test quat invert\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrVec3 axis{1,1,0};
        XrQuat q = m->quatFromAxisAngle(axis, 0.7f);
        XrQuat inv = m->quatInvert(q);
        XrQuat prod = m->quatMultiply(q, inv);
        CHECK_NEAR(prod.x, 0.0f, 0.001f, "q*q^-1 x");
        CHECK_NEAR(prod.y, 0.0f, 0.001f, "q*q^-1 y");
        CHECK_NEAR(prod.z, 0.0f, 0.001f, "q*q^-1 z");
        CHECK_NEAR(prod.w, 1.0f, 0.001f, "q*q^-1 w");
    }

    // 7. Quaternion normalize
    std::printf("[xr_math] test quat normalize\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrQuat q{1,2,3,4};
        XrQuat n = m->quatNormalize(q);
        float len = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z + n.w*n.w);
        CHECK_NEAR(len, 1.0f, 0.001f, "normalized length=1");
    }

    // 8. Quaternion slerp
    std::printf("[xr_math] test quat slerp\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrQuat a{0,0,0,1}; // identity
        XrVec3 z{0,0,1};
        XrQuat b = m->quatFromAxisAngle(z, 3.14159265f); // 180°
        XrQuat mid = m->quatSlerp(a, b, 0.5f);
        // midpoint of 0→180° should be 90°
        XrVec3 v{1,0,0};
        XrVec3 result = m->quatRotateVector(mid, v);
        CHECK_NEAR(result.x, 0.0f, 0.02f, "slerp midpoint x");
        CHECK_NEAR(result.y, 1.0f, 0.02f, "slerp midpoint y");
    }

    // 9. Vector operations
    std::printf("[xr_math] test vector ops\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrVec3 a{1,2,3}, b{4,5,6};
        XrVec3 sum = m->vecAdd(a, b);
        CHECK_NEAR(sum.x, 5.0f, 1e-6f, "vecAdd x");
        CHECK_NEAR(sum.z, 9.0f, 1e-6f, "vecAdd z");
        XrVec3 diff = m->vecSub(b, a);
        CHECK_NEAR(diff.x, 3.0f, 1e-6f, "vecSub x");
        float dot = m->vecDot(a, b);
        CHECK_NEAR(dot, 32.0f, 1e-5f, "vecDot");
        XrVec3 cr = m->vecCross(XrVec3{1,0,0}, XrVec3{0,1,0});
        CHECK_NEAR(cr.x, 0.0f, 1e-6f, "cross x");
        CHECK_NEAR(cr.y, 0.0f, 1e-6f, "cross y");
        CHECK_NEAR(cr.z, 1.0f, 1e-6f, "cross z");
        XrVec3 n = m->vecNormalize(XrVec3{3,0,4});
        CHECK_NEAR(n.x, 0.6f, 1e-5f, "normalize x");
        CHECK_NEAR(n.z, 0.8f, 1e-5f, "normalize z");
    }

    // 10. Pose identity
    std::printf("[xr_math] test pose identity\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrPose id = m->poseIdentity();
        CHECK_NEAR(id.position.x, 0.0f, 1e-6f, "pose id pos x");
        CHECK_NEAR(id.orientation.w, 1.0f, 1e-6f, "pose id quat w");
    }

    // 11. Pose multiply (A*B)*v = A*(B*v)
    std::printf("[xr_math] test pose multiply\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrPose a = m->poseIdentity();
        a.position = {1,0,0};
        XrPose b = m->poseIdentity();
        b.position = {0,2,0};
        XrPose ab = m->poseMultiply(a, b);
        CHECK_NEAR(ab.position.x, 1.0f, 0.001f, "pose mul pos x");
        CHECK_NEAR(ab.position.y, 2.0f, 0.001f, "pose mul pos y");
        CHECK_NEAR(ab.position.z, 0.0f, 0.001f, "pose mul pos z");
    }

    // 12. Pose invert (p * p^-1 = identity)
    std::printf("[xr_math] test pose invert\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrPose p;
        p.position = {3,4,5};
        XrVec3 z{0,0,1};
        p.orientation = m->quatFromAxisAngle(z, 1.2f);
        XrPose inv = m->poseInvert(p);
        XrPose product = m->poseMultiply(p, inv);
        CHECK_NEAR(product.position.x, 0.0f, 0.001f, "pose*p^-1 pos x");
        CHECK_NEAR(product.position.y, 0.0f, 0.001f, "pose*p^-1 pos y");
        CHECK_NEAR(product.position.z, 0.0f, 0.001f, "pose*p^-1 pos z");
        CHECK_NEAR(product.orientation.w, 1.0f, 0.001f, "pose*p^-1 quat w");
    }

    // 13. Pose transform point
    std::printf("[xr_math] test pose transform point\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrPose p = m->poseIdentity();
        p.position = {10,0,0};
        XrVec3 v{1,0,0};
        XrVec3 result = m->poseTransformPoint(p, v);
        CHECK_NEAR(result.x, 11.0f, 0.001f, "transform point x");
    }

    // 14. Matrix from pose round-trip
    std::printf("[xr_math] test mat4 round-trip\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrPose p;
        p.position = {5,6,7};
        XrVec3 axis{0,1,0};
        p.orientation = m->quatFromAxisAngle(axis, 0.8f);
        XrMat4 mat = m->mat4FromPose(p);
        CHECK_NEAR(mat.m[12], 5.0f, 0.001f, "mat4 translation x");
        CHECK_NEAR(mat.m[13], 6.0f, 0.001f, "mat4 translation y");
        CHECK_NEAR(mat.m[14], 7.0f, 0.001f, "mat4 translation z");
        CHECK_NEAR(mat.m[15], 1.0f, 1e-5f, "mat4 homogeneous");
        // Pose from mat4
        XrPose p2 = m->poseFromMat4(mat);
        CHECK_NEAR(p2.position.x, 5.0f, 0.001f, "poseFromMat4 pos x");
        CHECK_NEAR(p2.position.y, 6.0f, 0.001f, "poseFromMat4 pos y");
        CHECK_NEAR(p2.position.z, 7.0f, 0.001f, "poseFromMat4 pos z");
        // Quat should match
        CHECK_QUAT_NEAR(p.orientation, p2.orientation, 0.01f, "poseFromMat4 quat");
    }

    // 15. Matrix multiply associativity
    std::printf("[xr_math] test mat4 multiply\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrMat4 a = m->mat4FromPose(m->poseIdentity());
        a.m[12] = 1; // translate x=1
        XrMat4 b = m->mat4FromPose(m->poseIdentity());
        b.m[13] = 2; // translate y=2
        XrMat4 ab = m->mat4Multiply(a, b);
        CHECK_NEAR(ab.m[12], 1.0f, 0.001f, "matMul translation x");
        CHECK_NEAR(ab.m[13], 2.0f, 0.001f, "matMul translation y");
    }

    // 16. Matrix invert (m * m^-1 = I)
    std::printf("[xr_math] test mat4 invert\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrPose p;
        p.position = {3,4,5};
        XrVec3 axis{1,0,0};
        p.orientation = m->quatFromAxisAngle(axis, 0.5f);
        XrMat4 mat = m->mat4FromPose(p);
        XrMat4 inv = m->mat4Invert(mat);
        XrMat4 identity = m->mat4Multiply(mat, inv);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                float expected = (i == j) ? 1.0f : 0.0f;
                CHECK_NEAR(identity.m[j*4+i], expected, 0.001f, "m*m^-1 = I");
            }
    }

    // 17. Quaternion rotation consistency (rotating around X axis)
    std::printf("[xr_math] test rotation consistency\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrVec3 x{1,0,0};
        XrQuat q = m->quatFromAxisAngle(x, 3.14159265f / 2.0f);
        XrVec3 v{0,1,0};
        XrVec3 result = m->quatRotateVector(q, v);
        // 90° around X: (0,1,0) → (0,0,1)
        CHECK_NEAR(result.x, 0.0f, 0.01f, "rotX 90 x");
        CHECK_NEAR(result.y, 0.0f, 0.01f, "rotX 90 y");
        CHECK_NEAR(result.z, 1.0f, 0.01f, "rotX 90 z");
    }

    // 18. Pose chaining (A*B)*v == A*(B*v)
    std::printf("[xr_math] test pose chaining\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrPose a;
        a.position = {1,0,0};
        XrVec3 z{0,0,1};
        a.orientation = m->quatFromAxisAngle(z, 3.14159265f / 2.0f);
        XrPose b;
        b.position = {0,2,0};
        XrPose ab = m->poseMultiply(a, b);
        XrVec3 v{1,0,0};
        XrVec3 r1 = m->poseTransformPoint(ab, v);
        XrVec3 tmp = m->poseTransformPoint(b, v);
        XrVec3 r2 = m->poseTransformPoint(a, tmp);
        CHECK_NEAR(r1.x, r2.x, 0.001f, "chain x");
        CHECK_NEAR(r1.y, r2.y, 0.001f, "chain y");
        CHECK_NEAR(r1.z, r2.z, 0.001f, "chain z");
    }

    // 19. Determinism
    std::printf("[xr_math] test determinism\n");
    {
        std::string err;
        auto m = create_xr_math(XrConfig{}, err);
        XrVec3 axis{1,2,3};
        XrQuat q1 = m->quatFromAxisAngle(axis, 1.5f);
        XrQuat q2 = m->quatFromAxisAngle(axis, 1.5f);
        CHECK(q1.x == q2.x && q1.y == q2.y && q1.z == q2.z && q1.w == q2.w, "deterministic quat");
        XrPose p1 = m->poseMultiply(m->poseIdentity(), m->poseIdentity());
        XrPose p2 = m->poseMultiply(m->poseIdentity(), m->poseIdentity());
        CHECK(p1.position.x == p2.position.x, "deterministic pose");
    }

    // 20. Refusals (NaN in config)
    std::printf("[xr_math] test refusals\n");
    {
        XrConfig bad; bad.epsilon = std::nanf("");
        std::string err;
        auto p = create_xr_math(bad, err);
        CHECK(p == nullptr, "NaN epsilon refused");
    }

    std::printf("\n[xr_math] Results: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed == 0) std::printf("[xr_math] ALL PASSED\n");
    return g_failed == 0 ? 0 : 1;
}
