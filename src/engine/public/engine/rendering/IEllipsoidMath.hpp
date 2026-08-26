// IEllipsoidMath — Agente 1 (task_plan C.2 cesium-native): the PUBLIC
// ellipsoid math contract. Headless, deterministic, self-contained std+glm.
//
// Geodetic ↔ ECEF (Earth-Centered, Earth-Fixed) coordinate conversions,
// ENU (East-North-Up) local frames, and cartographic operations.
// Based on WGS84 ellipsoid (a=6378137, b=6356752.3142).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <glm/vec3.hpp>

namespace vc::rendering {

struct EllipsoidConfig {
    // Semi-major axis (equatorial radius) in meters. Default WGS84.
    double semiMajorAxis = 6378137.0;
    // Semi-minor axis (polar radius) in meters. Default WGS84.
    double semiMinorAxis = 6356752.314245179;

    bool validate() const;
    std::string toJson() const;
    static EllipsoidConfig fromJson(const std::string& json, std::string& errorOut);
};

// Geodetic coordinates: longitude/latitude/radians + height in meters.
struct Geodetic {
    double longitude = 0.0; // radians, [-π, π]
    double latitude  = 0.0; // radians, [-π/2, π/2]
    double height    = 0.0; // meters above ellipsoid

    bool operator==(const Geodetic& o) const;
    bool operator!=(const Geodetic& o) const { return !(*this == o); }
};

// ECEF position in meters.
struct EcefPosition {
    double x = 0.0, y = 0.0, z = 0.0;

    bool operator==(const EcefPosition& o) const;
    bool operator!=(const EcefPosition& o) const { return !(*this == o); }
};

class IEllipsoidMath {
public:
    virtual ~IEllipsoidMath() = default;

    // Geodetic → ECEF conversion.
    virtual EcefPosition geodeticToEcef(const Geodetic& geo) const = 0;

    // ECEF → Geodetic conversion (iterative, converges in ~3 iterations).
    virtual Geodetic ecefToGeodetic(const EcefPosition& ecef) const = 0;

    // Compute ENU (East-North-Up) basis vectors at a geodetic location.
    // Returns: east, north, up as unit vectors in ECEF frame.
    virtual void enuBasis(const Geodetic& geo,
                          glm::dvec3& east, glm::dvec3& north, glm::dvec3& up) const = 0;

    // Compute the surface normal at a geodetic point (unit vector in ECEF).
    virtual glm::dvec3 surfaceNormal(const Geodetic& geo) const = 0;

    // Compute the distance between two geodetic points (great-circle approximation).
    virtual double geodeticDistance(const Geodetic& a, const Geodetic& b) const = 0;

    // Compute the geodetic longitude/latitude range for a tile at given level.
    // Returns west/south/east/north in radians.
    virtual void tileBounds(int tileX, int tileY, int level,
                            double& west, double& south, double& east, double& north) const = 0;
};

std::unique_ptr<IEllipsoidMath> create_ellipsoid_math(
    const EllipsoidConfig& config, std::string& errorOut);

} // namespace vc::rendering
