// EllipsoidMathTests.cpp — gate for IEllipsoidMath (C.2 cesium-native)
// Headless, deterministic, no GPU required.

#include "engine/rendering/IEllipsoidMath.hpp"
#include <cstdio>
#include <cmath>

using namespace vc::rendering;

static int g_passed = 0, g_failed = 0;
#define CHECK(cond, msg) do { if(!(cond)){std::printf("  FAIL: %s\n",msg);g_failed++;}else g_passed++; }while(0)
#define CHECK_NEAR(a, b, eps, msg) CHECK(std::fabs((a)-(b)) < (eps), msg)

static constexpr double PI = 3.14159265358979323846;

int main() {
    std::printf("[ellipsoid] ALL tests starting\n");

    // 1. Config.
    std::printf("[ellipsoid] test config\n");
    { EllipsoidConfig c; c.semiMajorAxis = 0; CHECK(!c.validate(), "zero invalid"); }
    { EllipsoidConfig c; c.semiMinorAxis = -1; CHECK(!c.validate(), "negative invalid"); }
    { EllipsoidConfig c; c.semiMinorAxis = 7000000; CHECK(!c.validate(), "minor>major invalid"); }
    { EllipsoidConfig c; CHECK(c.validate(), "default valid"); }

    // 2. JSON round-trip.
    std::printf("[ellipsoid] test JSON\n");
    {
        EllipsoidConfig c; c.semiMajorAxis = 6400000.0;
        std::string json = c.toJson();
        std::string err;
        auto r = EllipsoidConfig::fromJson(json, err);
        CHECK(err.empty(), "no error");
        CHECK_NEAR(r.semiMajorAxis, 6400000.0, 1.0, "semiMajor round-trip");
    }

    // 3. Create.
    std::printf("[ellipsoid] test create\n");
    std::string err;
    EllipsoidConfig cfg;
    auto em = create_ellipsoid_math(cfg, err);
    CHECK(em != nullptr, "created");

    // 4. Geodetic→ECEF: prime meridian, equator.
    std::printf("[ellipsoid] test geodetic→ECEF\n");
    {
        Geodetic g; g.longitude = 0; g.latitude = 0; g.height = 0;
        auto e = em->geodeticToEcef(g);
        CHECK_NEAR(e.x, 6378137.0, 1.0, "equator X = semiMajor");
        CHECK_NEAR(e.y, 0.0, 1.0, "equator Y = 0");
        CHECK_NEAR(e.z, 0.0, 1.0, "equator Z = 0");
    }

    // 5. Geodetic→ECEF: north pole.
    std::printf("[ellipsoid] test north pole\n");
    {
        Geodetic g; g.longitude = 0; g.latitude = PI/2; g.height = 0;
        auto e = em->geodeticToEcef(g);
        CHECK_NEAR(e.x, 0.0, 1.0, "north pole X=0");
        CHECK_NEAR(e.y, 0.0, 1.0, "north pole Y=0");
        CHECK_NEAR(e.z, 6356752.3142, 1.0, "north pole Z = semiMinor");
    }

    // 6. Round-trip: geodetic→ECEF→geodetic.
    std::printf("[ellipsoid] test round-trip\n");
    {
        Geodetic g; g.longitude = 1.234; g.latitude = -0.567; g.height = 1234.0;
        auto e = em->geodeticToEcef(g);
        auto g2 = em->ecefToGeodetic(e);
        CHECK_NEAR(g2.longitude, g.longitude, 1e-8, "lon round-trip");
        CHECK_NEAR(g2.latitude, g.latitude, 1e-8, "lat round-trip");
        CHECK_NEAR(g2.height, g.height, 0.01, "height round-trip");
    }

    // 7. Round-trip at multiple locations.
    std::printf("[ellipsoid] test multiple round-trips\n");
    {
        double lons[] = {0, 1.5, -2.1, PI};
        double lats[] = {0, 0.7, -0.3, PI/4};
        double hs[] = {0, 500, 10000, -100};
        for (int i = 0; i < 4; i++) {
            Geodetic g; g.longitude = lons[i]; g.latitude = lats[i]; g.height = hs[i];
            auto e = em->geodeticToEcef(g);
            auto g2 = em->ecefToGeodetic(e);
            CHECK_NEAR(g2.longitude, g.longitude, 1e-8, "multi lon");
            CHECK_NEAR(g2.latitude, g.latitude, 1e-8, "multi lat");
            CHECK_NEAR(g2.height, g.height, 0.1, "multi height");
        }
    }

    // 8. Surface normal at equator.
    std::printf("[ellipsoid] test surface normal\n");
    {
        Geodetic g; g.longitude = 0; g.latitude = 0; g.height = 0;
        auto n = em->surfaceNormal(g);
        CHECK_NEAR(n.x, 1.0, 0.01, "equator normal X≈1");
        CHECK_NEAR(n.y, 0.0, 0.01, "equator normal Y≈0");
        CHECK_NEAR(n.z, 0.0, 0.01, "equator normal Z≈0");
    }

    // 9. Surface normal at north pole.
    std::printf("[ellipsoid] test normal north pole\n");
    {
        Geodetic g; g.longitude = 0; g.latitude = PI/2; g.height = 0;
        auto n = em->surfaceNormal(g);
        CHECK_NEAR(n.x, 0.0, 0.01, "pole normal X≈0");
        CHECK_NEAR(n.y, 0.0, 0.01, "pole normal Y≈0");
        CHECK_NEAR(n.z, 1.0, 0.01, "pole normal Z≈1");
    }

    // 10. ENU basis at equator.
    std::printf("[ellipsoid] test ENU basis\n");
    {
        Geodetic g; g.longitude = 0; g.latitude = 0; g.height = 0;
        glm::dvec3 east, north, up;
        em->enuBasis(g, east, north, up);
        CHECK_NEAR(east.x, 0.0, 0.01, "equator east X=0");
        CHECK_NEAR(east.y, 1.0, 0.01, "equator east Y=1");
        CHECK_NEAR(north.x, 0.0, 0.01, "equator north X=0");
        CHECK_NEAR(north.z, 1.0, 0.01, "equator north Z=1");
    }

    // 11. Geodetic distance: same point = 0.
    std::printf("[ellipsoid] test distance\n");
    {
        Geodetic g; g.longitude = 1.0; g.latitude = 0.5; g.height = 0;
        double d = em->geodeticDistance(g, g);
        CHECK_NEAR(d, 0.0, 0.01, "self distance = 0");
    }

    // 12. Geodetic distance: ~1 degree at equator ≈ 111km.
    std::printf("[ellipsoid] test distance 1deg\n");
    {
        Geodetic a; a.longitude = 0; a.latitude = 0; a.height = 0;
        Geodetic b; b.longitude = PI/180.0; b.latitude = 0; b.height = 0;
        double d = em->geodeticDistance(a, b);
        CHECK_NEAR(d, 111319.5, 500.0, "1deg lon at equator ≈ 111km");
    }

    // 13. Tile bounds.
    std::printf("[ellipsoid] test tile bounds\n");
    {
        double w, s, e, n;
        em->tileBounds(0, 0, 1, w, s, e, n);
        CHECK_NEAR(w, -PI, 0.01, "tile (0,0,l1) west=-PI");
        CHECK_NEAR(e, 0.0, 0.01, "tile (0,0,l1) east=0");
        CHECK(n > 0, "tile (0,0,l1) north > 0");
        CHECK(s <= 0, "tile (0,0,l1) south <= 0");
        CHECK(n > s, "tile (0,0,l1) north > south");
    }

    // 14. Determinism.
    std::printf("[ellipsoid] test determinism\n");
    {
        Geodetic g; g.longitude = 2.34; g.latitude = -0.78; g.height = 500;
        auto e1 = em->geodeticToEcef(g);
        auto e2 = em->geodeticToEcef(g);
        CHECK(e1.x == e2.x && e1.y == e2.y && e1.z == e2.z, "deterministic ECEF");
    }

    std::printf("\n[ellipsoid] Results: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed > 0) { std::printf("[ellipsoid] FAILED\n"); return 1; }
    std::printf("[ellipsoid] ALL PASSED\n");
    return 0;
}
