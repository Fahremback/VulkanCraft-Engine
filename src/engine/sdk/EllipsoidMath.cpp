// EllipsoidMath.cpp — adapter for IEllipsoidMath.
// WGS84 ellipsoid: geodetic ↔ ECEF, ENU, tile bounds.
// Headless, deterministic, pure math, no GPU required.

#include "engine/rendering/IEllipsoidMath.hpp"
#include <cmath>
#include <sstream>

namespace vc::rendering {

static constexpr double PI = 3.14159265358979323846;

bool Geodetic::operator==(const Geodetic& o) const {
    // Compare component-wise. Radians/heights are exact doubles, so an exact
    // comparison is meaningful; consumers doing fuzzy tolerance checks compare
    // the numeric fields themselves.
    return longitude == o.longitude && latitude == o.latitude && height == o.height;
}

bool EcefPosition::operator==(const EcefPosition& o) const {
    return x == o.x && y == o.y && z == o.z;
}

// ─── Config ───────────────────────────────────────

bool EllipsoidConfig::validate() const {
    return semiMajorAxis > 0.0 && semiMinorAxis > 0.0 && semiMinorAxis <= semiMajorAxis;
}

std::string EllipsoidConfig::toJson() const {
    std::ostringstream o;
    o << "{\"semiMajorAxis\":" << semiMajorAxis << ",\"semiMinorAxis\":" << semiMinorAxis << "}";
    return o.str();
}

EllipsoidConfig EllipsoidConfig::fromJson(const std::string& json, std::string& err) {
    EllipsoidConfig c;
    auto p = json.find("\"semiMajorAxis\"");
    if (p != std::string::npos) { p = json.find(':', p + 14); if (p != std::string::npos) c.semiMajorAxis = std::strtod(json.c_str() + p + 1, nullptr); }
    p = json.find("\"semiMinorAxis\"");
    if (p != std::string::npos) { p = json.find(':', p + 14); if (p != std::string::npos) c.semiMinorAxis = std::strtod(json.c_str() + p + 1, nullptr); }
    if (!c.validate()) { err = "invalid config"; return {}; }
    return c;
}

// ─── Adapter ──────────────────────────────────────

class EllipsoidMathImpl : public IEllipsoidMath {
public:
    explicit EllipsoidMathImpl(const EllipsoidConfig& cfg)
        : a_(cfg.semiMajorAxis), b_(cfg.semiMinorAxis),
          a2_(cfg.semiMajorAxis * cfg.semiMajorAxis),
          b2_(cfg.semiMinorAxis * cfg.semiMinorAxis),
          e2_((a2_ - b2_) / a2_) {} // Eccentricity squared.

    EcefPosition geodeticToEcef(const Geodetic& geo) const override {
        double cosLat = std::cos(geo.latitude);
        double sinLat = std::sin(geo.latitude);
        double cosLon = std::cos(geo.longitude);
        double sinLon = std::sin(geo.longitude);
        // Prime vertical radius of curvature.
        double N = a_ / std::sqrt(1.0 - e2_ * sinLat * sinLat);
        double h = geo.height;
        return {
            (N + h) * cosLat * cosLon,
            (N + h) * cosLat * sinLon,
            (N * (1.0 - e2_) + h) * sinLat
        };
    }

    Geodetic ecefToGeodetic(const EcefPosition& ecef) const override {
        // Iterative Bowring method.
        double p = std::sqrt(ecef.x * ecef.x + ecef.y * ecef.y);
        double lon = std::atan2(ecef.y, ecef.x);
        double lat = std::atan2(ecef.z, p * (1.0 - e2_)); // Initial estimate.

        for (int i = 0; i < 5; i++) {
            double sinLat = std::sin(lat);
            double N = a_ / std::sqrt(1.0 - e2_ * sinLat * sinLat);
            lat = std::atan2(ecef.z + e2_ * N * sinLat, p);
        }

        double sinLat = std::sin(lat);
        double N = a_ / std::sqrt(1.0 - e2_ * sinLat * sinLat);
        double cosLat = std::cos(lat);
        double height = (p / cosLat) - N;

        return { lon, lat, height };
    }

    void enuBasis(const Geodetic& geo,
                  glm::dvec3& east, glm::dvec3& north, glm::dvec3& up) const override {
        // ENU basis vectors at a geodetic point.
        double cosLat = std::cos(geo.latitude);
        double sinLat = std::sin(geo.latitude);
        double cosLon = std::cos(geo.longitude);
        double sinLon = std::sin(geo.longitude);

        // East = [-sinLon, cosLon, 0]
        east = glm::dvec3(-sinLon, cosLon, 0.0);

        // North = [-cosLon*sinLat, -sinLon*sinLat, cosLat]
        north = glm::dvec3(-cosLon * sinLat, -sinLon * sinLat, cosLat);

        // Up = surface normal (approximate: [cosLon*cosLat, sinLon*cosLat, sinLat])
        up = glm::dvec3(cosLon * cosLat, sinLon * cosLat, sinLat);
    }

    glm::dvec3 surfaceNormal(const Geodetic& geo) const override {
        double cosLat = std::cos(geo.latitude);
        double sinLat = std::sin(geo.latitude);
        double cosLon = std::cos(geo.longitude);
        double sinLon = std::sin(geo.longitude);
        // Normal direction (not unit, but proportional).
        double nx = cosLon * cosLat / a2_;
        double ny = sinLon * cosLat / a2_;
        double nz = sinLat / b2_;
        double len = std::sqrt(nx*nx + ny*ny + nz*nz);
        return glm::dvec3(nx/len, ny/len, nz/len);
    }

    double geodeticDistance(const Geodetic& a, const Geodetic& b) const override {
        // Vincenty approximation for distance.
        double dlat = b.latitude - a.latitude;
        double dlon = b.longitude - a.longitude;
        double meanLat = (a.latitude + b.latitude) * 0.5;
        double cosMean = std::cos(meanLat);
        double dx = dlon * cosMean;
        double dy = dlat;
        double meanR = (a_ + b_) * 0.5;
        return meanR * std::sqrt(dx*dx + dy*dy);
    }

    void tileBounds(int tileX, int tileY, int level,
                    double& west, double& south, double& east, double& north) const override {
        double n = std::pow(2.0, level);
        west  = (tileX / n) * 2.0 * PI - PI;
        east  = ((tileX + 1) / n) * 2.0 * PI - PI;
        // Web Mercator Y to latitude.
        north = std::atan(std::sinh(PI * (1.0 - 2.0 * tileY / n)));
        south = std::atan(std::sinh(PI * (1.0 - 2.0 * (tileY + 1) / n)));
    }

private:
    double a_, b_, a2_, b2_, e2_;
};

std::unique_ptr<IEllipsoidMath> create_ellipsoid_math(
    const EllipsoidConfig& config, std::string& errorOut) {
    if (!config.validate()) { errorOut = "invalid config"; return nullptr; }
    return std::make_unique<EllipsoidMathImpl>(config);
}

} // namespace vc::rendering
