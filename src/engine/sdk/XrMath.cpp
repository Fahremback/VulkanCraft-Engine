// XrMath.cpp — XR math adapter: quaternion, pose, matrix operations
// Pure CPU math, no OpenXR runtime required.

#include "engine/rendering/IXrMath.hpp"
#include <cmath>
#include <cstring>
#include <memory>

namespace vc::rendering {

static constexpr float PI = 3.14159265358979323846f;

class XrMathImpl : public IXrMath {
public:
    explicit XrMathImpl(const XrConfig& cfg) : cfg_(cfg) {}

    // ---- Quaternion ----
    XrQuat quatIdentity() const override { return {0,0,0,1}; }

    XrQuat quatFromAxisAngle(const XrVec3& axis, float angleRad) const override {
        float half = angleRad * 0.5f;
        float s = std::sin(half);
        float len = std::sqrt(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
        if (len < cfg_.epsilon) return {0,0,0,1};
        return { axis.x/len*s, axis.y/len*s, axis.z/len*s, std::cos(half) };
    }

    XrQuat quatMultiply(const XrQuat& a, const XrQuat& b) const override {
        return {
            a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
        };
    }

    XrQuat quatInvert(const XrQuat& q) const override {
        float len2 = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
        if (len2 < cfg_.epsilon) return {0,0,0,1};
        return { -q.x/len2, -q.y/len2, -q.z/len2, q.w/len2 };
    }

    XrQuat quatNormalize(const XrQuat& q) const override {
        float len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        if (len < cfg_.epsilon) return {0,0,0,1};
        return { q.x/len, q.y/len, q.z/len, q.w/len };
    }

    XrVec3 quatRotateVector(const XrQuat& q, const XrVec3& v) const override {
        // v' = v + 2w(u × v) + 2(u × (u × v))
        float ux = q.x, uy = q.y, uz = q.z, w = q.w;
        float cx = uy*v.z - uz*v.y;
        float cy = uz*v.x - ux*v.z;
        float cz = ux*v.y - uy*v.x;
        float dx = uy*cz - uz*cy;
        float dy = uz*cx - ux*cz;
        float dz = ux*cy - uy*cx;
        return {
            v.x + 2.0f*(w*cx + dx),
            v.y + 2.0f*(w*cy + dy),
            v.z + 2.0f*(w*cz + dz)
        };
    }

    XrQuat quatSlerp(const XrQuat& a, const XrQuat& b, float t) const override {
        float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
        XrQuat bb = b;
        if (dot < -cfg_.epsilon) { dot = -dot; bb = {-b.x,-b.y,-b.z,-b.w}; }
        if (dot > 1.0f - cfg_.epsilon) {
            return quatNormalize({a.x+t*(bb.x-a.x), a.y+t*(bb.y-a.y), a.z+t*(bb.z-a.z), a.w+t*(bb.w-a.w)});
        }
        float theta = std::acos(dot);
        float sinTheta = std::sin(theta);
        float wa = std::sin((1-t)*theta) / sinTheta;
        float wb = std::sin(t*theta) / sinTheta;
        return { wa*a.x+wb*bb.x, wa*a.y+wb*bb.y, wa*a.z+wb*bb.z, wa*a.w+wb*bb.w };
    }

    // ---- Vector ----
    XrVec3 vecAdd(const XrVec3& a, const XrVec3& b) const override { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
    XrVec3 vecSub(const XrVec3& a, const XrVec3& b) const override { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
    XrVec3 vecScale(const XrVec3& v, float s) const override { return {v.x*s, v.y*s, v.z*s}; }
    float vecDot(const XrVec3& a, const XrVec3& b) const override { return a.x*b.x + a.y*b.y + a.z*b.z; }
    XrVec3 vecCross(const XrVec3& a, const XrVec3& b) const override {
        return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
    }
    float vecLength(const XrVec3& v) const override { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
    XrVec3 vecNormalize(const XrVec3& v) const override {
        float len = vecLength(v);
        if (len < cfg_.epsilon) return {0,0,0};
        return {v.x/len, v.y/len, v.z/len};
    }
    XrVec3 vecLerp(const XrVec3& a, const XrVec3& b, float t) const override {
        return {a.x+t*(b.x-a.x), a.y+t*(b.y-a.y), a.z+t*(b.z-a.z)};
    }

    // ---- Pose ----
    XrPose poseIdentity() const override { return {quatIdentity(), {0,0,0}}; }

    XrPose poseMultiply(const XrPose& a, const XrPose& b) const override {
        return {
            quatMultiply(a.orientation, b.orientation),
            vecAdd(quatRotateVector(a.orientation, b.position), a.position)
        };
    }

    XrPose poseInvert(const XrPose& p) const override {
        XrQuat invQ = quatInvert(p.orientation);
        return { invQ, quatRotateVector(invQ, vecScale(p.position, -1.0f)) };
    }

    XrVec3 poseTransformPoint(const XrPose& p, const XrVec3& v) const override {
        return vecAdd(quatRotateVector(p.orientation, v), p.position);
    }

    // ---- Matrix ----
    XrMat4 mat4FromPose(const XrPose& p) const override {
        XrMat4 m{};
        const XrQuat& q = p.orientation;
        float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
        float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
        float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
        m.m[0]  = 1 - 2*(yy+zz); m.m[1]  = 2*(xy+wz);     m.m[2]  = 2*(xz-wy);     m.m[3]  = 0;
        m.m[4]  = 2*(xy-wz);     m.m[5]  = 1 - 2*(xx+zz); m.m[6]  = 2*(yz+wx);     m.m[7]  = 0;
        m.m[8]  = 2*(xz+wy);     m.m[9]  = 2*(yz-wx);     m.m[10] = 1 - 2*(xx+yy); m.m[11] = 0;
        m.m[12] = p.position.x;  m.m[13] = p.position.y;  m.m[14] = p.position.z;  m.m[15] = 1;
        return m;
    }

    XrPose poseFromMat4(const XrMat4& m) const override {
        XrPose p;
        p.position = {m.m[12], m.m[13], m.m[14]};
        float trace = m.m[0] + m.m[5] + m.m[10] + 1.0f;
        if (trace > 0) {
            float s = 0.5f / std::sqrt(trace);
            p.orientation.w = 0.25f / s;
            p.orientation.x = (m.m[6] - m.m[9]) * s;
            p.orientation.y = (m.m[8] - m.m[2]) * s;
            p.orientation.z = (m.m[1] - m.m[4]) * s;
        } else if (m.m[0] > m.m[5] && m.m[0] > m.m[10]) {
            float s = 2.0f * std::sqrt(1.0f + m.m[0] - m.m[5] - m.m[10]);
            p.orientation.w = (m.m[6] - m.m[9]) / s;
            p.orientation.x = 0.25f * s;
            p.orientation.y = (m.m[4] + m.m[1]) / s;
            p.orientation.z = (m.m[8] + m.m[2]) / s;
        } else if (m.m[5] > m.m[10]) {
            float s = 2.0f * std::sqrt(1.0f + m.m[5] - m.m[0] - m.m[10]);
            p.orientation.w = (m.m[8] - m.m[2]) / s;
            p.orientation.x = (m.m[4] + m.m[1]) / s;
            p.orientation.y = 0.25f * s;
            p.orientation.z = (m.m[9] + m.m[6]) / s;
        } else {
            float s = 2.0f * std::sqrt(1.0f + m.m[10] - m.m[0] - m.m[5]);
            p.orientation.w = (m.m[1] - m.m[4]) / s;
            p.orientation.x = (m.m[8] + m.m[2]) / s;
            p.orientation.y = (m.m[9] + m.m[6]) / s;
            p.orientation.z = 0.25f * s;
        }
        return p;
    }

    XrMat4 mat4Multiply(const XrMat4& a, const XrMat4& b) const override {
        XrMat4 r{};
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) {
                float sum = 0;
                for (int k = 0; k < 4; k++) sum += a.m[k*4+row] * b.m[c*4+k];
                r.m[c*4+row] = sum;
            }
        return r;
    }

    XrMat4 mat4Invert(const XrMat4& m) const override {
        // Cramer's rule for 4x4
        float inv[16], det;
        inv[0]  = m.m[5]*(m.m[10]*m.m[15]-m.m[11]*m.m[14]) - m.m[9]*(m.m[6]*m.m[15]-m.m[7]*m.m[14]) + m.m[13]*(m.m[6]*m.m[11]-m.m[7]*m.m[10]);
        inv[4]  = -m.m[4]*(m.m[10]*m.m[15]-m.m[11]*m.m[14]) + m.m[8]*(m.m[6]*m.m[15]-m.m[7]*m.m[14]) - m.m[12]*(m.m[6]*m.m[11]-m.m[7]*m.m[10]);
        inv[8]  = m.m[4]*(m.m[9]*m.m[15]-m.m[11]*m.m[13]) - m.m[8]*(m.m[5]*m.m[15]-m.m[7]*m.m[13]) + m.m[12]*(m.m[5]*m.m[11]-m.m[7]*m.m[9]);
        inv[12] = -m.m[4]*(m.m[9]*m.m[14]-m.m[10]*m.m[13]) + m.m[8]*(m.m[5]*m.m[14]-m.m[6]*m.m[13]) - m.m[12]*(m.m[5]*m.m[10]-m.m[6]*m.m[9]);
        inv[1]  = -m.m[1]*(m.m[10]*m.m[15]-m.m[11]*m.m[14]) + m.m[9]*(m.m[2]*m.m[15]-m.m[3]*m.m[14]) - m.m[13]*(m.m[2]*m.m[11]-m.m[3]*m.m[10]);
        inv[5]  = m.m[0]*(m.m[10]*m.m[15]-m.m[11]*m.m[14]) - m.m[8]*(m.m[2]*m.m[15]-m.m[3]*m.m[14]) + m.m[12]*(m.m[2]*m.m[11]-m.m[3]*m.m[10]);
        inv[9]  = -m.m[0]*(m.m[9]*m.m[15]-m.m[11]*m.m[13]) + m.m[8]*(m.m[1]*m.m[15]-m.m[3]*m.m[13]) - m.m[12]*(m.m[1]*m.m[11]-m.m[3]*m.m[9]);
        inv[13] = m.m[0]*(m.m[9]*m.m[14]-m.m[10]*m.m[13]) - m.m[8]*(m.m[1]*m.m[14]-m.m[2]*m.m[13]) + m.m[12]*(m.m[1]*m.m[10]-m.m[2]*m.m[9]);
        inv[2]  = m.m[1]*(m.m[6]*m.m[15]-m.m[7]*m.m[14]) - m.m[5]*(m.m[2]*m.m[15]-m.m[3]*m.m[14]) + m.m[13]*(m.m[2]*m.m[7]-m.m[3]*m.m[6]);
        inv[6]  = -m.m[0]*(m.m[6]*m.m[15]-m.m[7]*m.m[14]) + m.m[4]*(m.m[2]*m.m[15]-m.m[3]*m.m[14]) - m.m[12]*(m.m[2]*m.m[7]-m.m[3]*m.m[6]);
        inv[10] = m.m[0]*(m.m[5]*m.m[15]-m.m[7]*m.m[13]) - m.m[4]*(m.m[1]*m.m[15]-m.m[3]*m.m[13]) + m.m[12]*(m.m[1]*m.m[7]-m.m[3]*m.m[5]);
        inv[14] = -m.m[0]*(m.m[5]*m.m[14]-m.m[6]*m.m[13]) + m.m[4]*(m.m[1]*m.m[14]-m.m[2]*m.m[13]) - m.m[12]*(m.m[1]*m.m[6]-m.m[2]*m.m[5]);
        inv[3]  = -m.m[1]*(m.m[6]*m.m[11]-m.m[7]*m.m[10]) + m.m[5]*(m.m[2]*m.m[11]-m.m[3]*m.m[10]) - m.m[9]*(m.m[2]*m.m[7]-m.m[3]*m.m[6]);
        inv[7]  = m.m[0]*(m.m[6]*m.m[11]-m.m[7]*m.m[10]) - m.m[4]*(m.m[2]*m.m[11]-m.m[3]*m.m[10]) + m.m[8]*(m.m[2]*m.m[7]-m.m[3]*m.m[6]);
        inv[11] = -m.m[0]*(m.m[5]*m.m[11]-m.m[7]*m.m[9]) + m.m[4]*(m.m[1]*m.m[11]-m.m[3]*m.m[9]) - m.m[8]*(m.m[1]*m.m[7]-m.m[3]*m.m[5]);
        inv[15] = m.m[0]*(m.m[5]*m.m[10]-m.m[6]*m.m[9]) - m.m[4]*(m.m[1]*m.m[10]-m.m[2]*m.m[9]) + m.m[8]*(m.m[1]*m.m[6]-m.m[2]*m.m[5]);
        det = m.m[0]*inv[0] + m.m[1]*inv[4] + m.m[2]*inv[8] + m.m[3]*inv[12];
        XrMat4 r{};
        if (std::abs(det) < cfg_.epsilon) return r;
        for (int i = 0; i < 16; i++) r.m[i] = inv[i] / det;
        return r;
    }

    XrConfig getConfig() const override { return cfg_; }

private:
    XrConfig cfg_;
};

std::unique_ptr<IXrMath> create_xr_math(const XrConfig& config, std::string& errorOut) {
    if (!config.validate()) { errorOut = "invalid config"; return nullptr; }
    return std::make_unique<XrMathImpl>(config);
}

} // namespace vc::rendering
