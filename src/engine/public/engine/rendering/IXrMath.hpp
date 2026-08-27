#pragma once
// IXrMath.hpp — Headless XR math: quaternion, pose, matrix operations
// Wraps openxr-sdk-source xr_linear.h math as a self-contained contract.
// No GPU, no runtime, no OpenXR session required.

#include <memory>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace vc::rendering {

struct XrVec3 {
    float x = 0, y = 0, z = 0;
};

struct XrQuat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct XrPose {
    XrQuat orientation;
    XrVec3 position;
};

struct XrMat4 {
    float m[16]; // column-major
};

struct XrConfig {
    float epsilon = 1e-5f;

    bool validate() const {
        return epsilon > 0 && epsilon < 1.0f;
    }
    std::string toJson() const {
        return "{\"epsilon\":" + std::to_string(epsilon) + "}";
    }
    static XrConfig fromJson(const std::string& s) {
        XrConfig c;
        auto p = s.find("\"epsilon\":");
        if (p != std::string::npos) c.epsilon = std::stof(s.substr(p + 10));
        return c;
    }
};

class IXrMath {
public:
    virtual ~IXrMath() = default;

    // Quaternion operations
    virtual XrQuat quatIdentity() const = 0;
    virtual XrQuat quatFromAxisAngle(const XrVec3& axis, float angleRad) const = 0;
    virtual XrQuat quatMultiply(const XrQuat& a, const XrQuat& b) const = 0;
    virtual XrQuat quatInvert(const XrQuat& q) const = 0;
    virtual XrQuat quatNormalize(const XrQuat& q) const = 0;
    virtual XrVec3 quatRotateVector(const XrQuat& q, const XrVec3& v) const = 0;
    virtual XrQuat quatSlerp(const XrQuat& a, const XrQuat& b, float t) const = 0;

    // Vector operations
    virtual XrVec3 vecAdd(const XrVec3& a, const XrVec3& b) const = 0;
    virtual XrVec3 vecSub(const XrVec3& a, const XrVec3& b) const = 0;
    virtual XrVec3 vecScale(const XrVec3& v, float s) const = 0;
    virtual float vecDot(const XrVec3& a, const XrVec3& b) const = 0;
    virtual XrVec3 vecCross(const XrVec3& a, const XrVec3& b) const = 0;
    virtual float vecLength(const XrVec3& v) const = 0;
    virtual XrVec3 vecNormalize(const XrVec3& v) const = 0;
    virtual XrVec3 vecLerp(const XrVec3& a, const XrVec3& b, float t) const = 0;

    // Pose operations
    virtual XrPose poseIdentity() const = 0;
    virtual XrPose poseMultiply(const XrPose& a, const XrPose& b) const = 0;
    virtual XrPose poseInvert(const XrPose& p) const = 0;
    virtual XrVec3 poseTransformPoint(const XrPose& p, const XrVec3& v) const = 0;

    // Matrix operations
    virtual XrMat4 mat4FromPose(const XrPose& p) const = 0;
    virtual XrPose poseFromMat4(const XrMat4& m) const = 0;
    virtual XrMat4 mat4Multiply(const XrMat4& a, const XrMat4& b) const = 0;
    virtual XrMat4 mat4Invert(const XrMat4& m) const = 0;

    // Config
    virtual XrConfig getConfig() const = 0;
};

std::unique_ptr<IXrMath> create_xr_math(const XrConfig& config, std::string& errorOut);

} // namespace vc::rendering
