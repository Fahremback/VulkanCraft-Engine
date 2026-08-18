// wiMath.h — adapted from Wicked Engine (MIT, commit 2aa9fdf…).
// Original used DirectXMath; this adaptation implements the same API surface
// on GLM so the ported Wicked GUI runs on the VulkanCraft stack (Vulkan + glm,
// NO DirectX). Only the subset used by the ported GUI base/widgets is kept.
// See frontend/PORTS.md.
#pragma once

#include "CommonInclude.h"

#include <cmath>
#include <algorithm>
#include <limits>

// Minimal DirectX-compatible math types (plain structs — no DirectX headers).
// Named like DirectXMath so the ported Wicked code compiles unchanged.
#ifndef XM_PI
#define XM_PI 3.14159265358979323846f
#endif

struct XMFLOAT2 {
    float x = 0, y = 0;
    XMFLOAT2() = default;
    constexpr XMFLOAT2(float _x, float _y) : x(_x), y(_y) {}
};
struct XMFLOAT3 {
    float x = 0, y = 0, z = 0;
    XMFLOAT3() = default;
    constexpr XMFLOAT3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};
struct XMFLOAT4 {
    float x = 0, y = 0, z = 0, w = 0;
    XMFLOAT4() = default;
    constexpr XMFLOAT4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
};
// Row-major 4x4 (same member layout as DirectXMath's XMFLOAT4X4).
struct XMFLOAT4X4 {
    float _11, _12, _13, _14;
    float _21, _22, _23, _24;
    float _31, _32, _33, _34;
    float _41, _42, _43, _44;
    XMFLOAT4X4()
        : _11(0), _12(0), _13(0), _14(0), _21(0), _22(0), _23(0), _24(0),
          _31(0), _32(0), _33(0), _34(0), _41(0), _42(0), _43(0), _44(0) {}
    constexpr XMFLOAT4X4(float m11, float m12, float m13, float m14,
                         float m21, float m22, float m23, float m24,
                         float m31, float m32, float m33, float m34,
                         float m41, float m42, float m43, float m44)
        : _11(m11), _12(m12), _13(m13), _14(m14), _21(m21), _22(m22), _23(m23), _24(m24),
          _31(m31), _32(m32), _33(m33), _34(m34), _41(m41), _42(m42), _43(m43), _44(m44) {}
};

// The ported GUI stores/passes matrices; XMMATRIX is aliased to the plain
// row-major struct (glm handles the actual math in the render bridge).
using XMMATRIX = XMFLOAT4X4;

inline constexpr XMFLOAT4X4 IDENTITY_MATRIX = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);

// Left-handed orthographic projection (same convention as DirectXMath's
// XMMatrixOrthographicOffCenterLH), row-major.
inline XMMATRIX XMMatrixOrthographicOffCenterLH(float viewLeft, float viewRight,
                                                float viewBottom, float viewTop,
                                                float nearZ, float farZ) {
    const float rl = viewRight - viewLeft;
    const float tb = viewTop - viewBottom;
    const float fn = farZ - nearZ;
    return XMMATRIX(
        2.0f / rl, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f / tb, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f / fn, 0.0f,
        -(viewLeft + viewRight) / rl, -(viewTop + viewBottom) / tb, -nearZ / fn, 1.0f);
}

namespace wi::math {

inline constexpr float PI = XM_PI;

inline bool float_equal(float f1, float f2) {
    return std::fabs(f1 - f2) < std::numeric_limits<float>::epsilon();
}

constexpr float saturate(float x) {
    return std::max(0.0f, std::min(1.0f, x));
}

template<typename T>
inline constexpr T Lerp(const T& a, const T& b, float t) {
    return T(a + (b - a) * t);
}
inline constexpr float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}
inline constexpr XMFLOAT2 Lerp(const XMFLOAT2& a, const XMFLOAT2& b, float t) {
    return XMFLOAT2(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t));
}
inline constexpr XMFLOAT3 Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t) {
    return XMFLOAT3(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t));
}
inline constexpr XMFLOAT4 Lerp(const XMFLOAT4& a, const XMFLOAT4& b, float t) {
    return XMFLOAT4(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t), Lerp(a.w, b.w, t));
}

inline constexpr float InverseLerp(float value1, float value2, float pos) {
    return (pos - value1) / (value2 - value1);
}

template<typename T>
inline constexpr T Clamp(T x, T minValue, T maxValue) {
    return std::max(minValue, std::min(maxValue, x));
}
template<typename T>
inline constexpr T Min(T a, T b) { return a < b ? a : b; }
template<typename T>
inline constexpr T Max(T a, T b) { return a > b ? a : b; }
template<typename T>
inline constexpr T Abs(T x) { return x < T(0) ? -x : x; }

inline float LengthSquared(const XMFLOAT2& v) { return v.x * v.x + v.y * v.y; }
inline float LengthSquared(const XMFLOAT3& v) { return v.x * v.x + v.y * v.y + v.z * v.z; }
inline float Length(const XMFLOAT2& v) { return std::sqrt(LengthSquared(v)); }
inline float Length(const XMFLOAT3& v) { return std::sqrt(LengthSquared(v)); }

inline float Dot(const XMFLOAT2& v1, const XMFLOAT2& v2) {
    return v1.x * v2.x + v1.y * v2.y;
}
inline float Dot(const XMFLOAT3& v1, const XMFLOAT3& v2) {
    return v1.x * v2.x + v1.y * v2.y + v1.z * v2.z;
}

inline float Distance(const XMFLOAT2& v1, const XMFLOAT2& v2) {
    return Length(XMFLOAT2(v1.x - v2.x, v1.y - v2.y));
}
inline float Distance(const XMFLOAT3& v1, const XMFLOAT3& v2) {
    return Length(XMFLOAT3(v1.x - v2.x, v1.y - v2.y, v1.z - v2.z));
}
inline float DistanceSquared(const XMFLOAT2& v1, const XMFLOAT2& v2) {
    return LengthSquared(XMFLOAT2(v1.x - v2.x, v1.y - v2.y));
}
inline float DistanceSquared(const XMFLOAT3& v1, const XMFLOAT3& v2) {
    return LengthSquared(XMFLOAT3(v1.x - v2.x, v1.y - v2.y, v1.z - v2.z));
}

inline XMFLOAT3 Normalize(const XMFLOAT3& v) {
    const float len = Length(v);
    return len > 0.0f ? XMFLOAT3(v.x / len, v.y / len, v.z / len) : XMFLOAT3(0, 0, 0);
}
inline XMFLOAT3 Cross(const XMFLOAT3& v1, const XMFLOAT3& v2) {
    return XMFLOAT3(
        v1.y * v2.z - v1.z * v2.y,
        v1.z * v2.x - v1.x * v2.z,
        v1.x * v2.y - v1.y * v2.x);
}

} // namespace wi::math
