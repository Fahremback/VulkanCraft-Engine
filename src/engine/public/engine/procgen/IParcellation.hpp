#pragma once

// Public parceling / road-graph / polygon-triangulation contracts
// (META section 18 / FALTANTES item 14).
//
// The parceling pipeline is data-driven and deterministic: a road network is
// built as a Delaunay triangulation of a set of junction points (roads = the
// selected Delaunay edges, optionally filtered by length), the network
// partitions the plane into polygonal parcels (the bounded faces of the
// planar subdivision — parcels with interior road loops come back with hole
// rings) and every parcel can be triangulated into a flat index list for
// mesh cooking. Same contract family as INoiseGraph / IClimateBiome:
// self-contained, never leaks the backend.
//
// Coordinates are world-space XZ/XY doubles (y-up convention: a CCW ring has
// positive shoelace area). Everything is deterministic — Delaunay, face
// extraction and triangulation are pure functions of the input, so two
// builds with the same asset produce identical output.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace procgen {

// A 2D point in parcel space.
struct ParcelPoint {
    double x{ 0.0 };
    double y{ 0.0 };

    bool operator==(const ParcelPoint& o) const {
        return x == o.x && y == o.y;
    }
};

// An undirected road edge between two junction points (indices into the
// network's point list; always a Delaunay edge of that point set).
struct RoadEdge {
    std::uint32_t a{ 0 };
    std::uint32_t b{ 0 };

    bool operator==(const RoadEdge& o) const {
        return a == o.a && b == o.b;
    }
};

// A road network: the junction points plus the selected Delaunay edges.
struct RoadNetwork {
    std::vector<ParcelPoint> points;
    std::vector<RoadEdge> edges;
};

// Asset spec for building a road network. `maxEdgeLength` selects the subset
// of Delaunay edges no longer than the given length (0 = keep every edge).
// Selection is deterministic: edges are sorted by length, then by endpoint
// indices.
struct RoadNetworkSpec {
    std::vector<ParcelPoint> points;
    double maxEdgeLength{ 0.0 };
};

// Builds RoadNetwork assets from specs, with versioned JSON serialization
// (all-or-nothing: a failed deserialize preserves the previous state).
class IRoadNetworkBuilder {
public:
    virtual ~IRoadNetworkBuilder() = default;

    // Builds the network for `spec`. Returns false with a message on invalid
    // input (fewer than three points, degenerate triangulation).
    virtual bool build(const RoadNetworkSpec& spec, std::string& errorOut) = 0;

    // The network produced by the last successful build.
    virtual const RoadNetwork& network() const = 0;

    virtual bool serialize(std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// One parcel: a simple outer ring (CCW, positive shoelace area) with zero or
// more hole rings (CW, negative shoelace area).
struct ParcelPolygon {
    std::vector<ParcelPoint> outer;
    std::vector<std::vector<ParcelPoint>> holes;

    // Shoelace signed area (positive = CCW in the y-up convention).
    static double ring_area(const std::vector<ParcelPoint>& ring);

    // Parcel area: outer ring minus holes.
    double area() const;

    // Point-in-parcel: inside the outer ring and outside every hole.
    bool contains(const ParcelPoint& p) const;
};

// Extracts parcels (bounded faces) from a road network and triangulates
// arbitrary parcel polygons into flat index lists.
class IParcellation {
public:
    virtual ~IParcellation() = default;

    // Bounded faces of the planar subdivision defined by the road edges.
    // Faces with interior road loops come back with hole rings; degenerate
    // (zero-area) faces are dropped. Edges not present in the Delaunay
    // triangulation of the point set are ignored (a road network built by
    // IRoadNetworkBuilder is always a valid subset). Deterministic.
    virtual bool parcels_from_network(const RoadNetwork& network,
                                      std::vector<ParcelPolygon>& out,
                                      std::string& errorOut) = 0;

    // Triangulates `parcel` (outer + holes) into a flat index list of groups
    // of three; indices reference the concatenated ring vertices
    // [outer..., hole0..., hole1..., ...], winding matches the input rings.
    // Deterministic.
    virtual bool triangulate(const ParcelPolygon& parcel,
                             std::vector<std::uint32_t>& out,
                             std::string& errorOut) = 0;
};

// Factories (implemented by the SDK adapter — the only TU that touches the
// delaunator/earcut clones).
std::shared_ptr<IRoadNetworkBuilder> create_road_network_builder();
std::shared_ptr<IParcellation> create_parcellation();

}  // namespace procgen
}  // namespace engine
