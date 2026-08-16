// Parcellation.cpp
//
// SDK adapter for engine/procgen/IParcellation.hpp (META section 18 /
// FALTANTES item 14: parceling/road graph with Delaunay + polygon
// triangulation). The public contract never leaks the backend; this TU is the
// ONLY one that includes the promoted clones: delaunator (Volodymyr
// Bilonenko, MIT) and earcut.hpp (Mapbox, ISC) — gate §32, see
// DEPENDENCY_POLICY.
//
// Model: junctions -> Delaunay triangulation (delaunator) -> roads = the
// selected Delaunay edges (optionally filtered by length, deterministic
// order) -> parcels = the bounded faces of the planar subdivision of the road
// network, extracted by flood-filling the triangle adjacency across
// non-road edges and walking each face's boundary (holes appear when roads
// form interior loops) -> earcut triangulates any parcel (outer + holes)
// into a flat index list for mesh cooking.
//
// Determinism: delaunator and earcut are pure functions of the input; face
// extraction uses only deterministic orderings (triangle index order,
// sorted road keys), so two builds with the same asset produce identical
// road networks, parcels and triangulations.

#include "engine/procgen/IParcellation.hpp"

#include "RegistryJson.hpp"

#include "delaunator.hpp"
#include "mapbox/earcut.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace procgen {

namespace {

// Halfedge helpers (delaunator convention: triangles are CCW, halfedge e
// runs triangles[e] -> triangles[next_halfedge(e)]).
std::size_t next_halfedge(std::size_t e) {
    return (e % 3 == 2) ? e - 2 : e + 1;
}

constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();
constexpr double kAreaEpsilon = 1e-9;

double ring_area_impl(const std::vector<ParcelPoint>& ring) {
    double area = 0.0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const ParcelPoint& a = ring[i];
        const ParcelPoint& b = ring[(i + 1) % ring.size()];
        area += a.x * b.y - b.x * a.y;
    }
    return area * 0.5;
}

// Canonical undirected edge key (min, max) -> single sorted vector for
// deterministic binary-search lookups.
struct RoadKey {
    std::uint32_t lo{ 0 };
    std::uint32_t hi{ 0 };
};

bool operator<(const RoadKey& a, const RoadKey& b) {
    return a.lo < b.lo || (a.lo == b.lo && a.hi < b.hi);
}
bool operator==(const RoadKey& a, const RoadKey& b) {
    return a.lo == b.lo && a.hi == b.hi;
}

std::vector<RoadKey> make_road_keys(const std::vector<RoadEdge>& edges) {
    std::vector<RoadKey> keys;
    keys.reserve(edges.size());
    for (const RoadEdge& e : edges) {
        keys.push_back({ std::min(e.a, e.b), std::max(e.a, e.b) });
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

bool has_road(const std::vector<RoadKey>& keys, std::uint32_t a,
              std::uint32_t b) {
    const RoadKey key{ std::min(a, b), std::max(a, b) };
    return std::binary_search(keys.begin(), keys.end(), key);
}

bool has_duplicate_points(const std::vector<ParcelPoint>& points) {
    std::vector<ParcelPoint> sorted = points;
    std::sort(sorted.begin(), sorted.end(),
              [](const ParcelPoint& a, const ParcelPoint& b) {
                  if (a.x != b.x) {
                      return a.x < b.x;
                  }
                  return a.y < b.y;
              });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i].x == sorted[i - 1].x && sorted[i].y == sorted[i - 1].y) {
            return true;
        }
    }
    return false;
}

// Removes road edges that dangle (an endpoint with road-degree 1): a bridge
// borders the same face on both sides and contributes no bounded parcel.
std::vector<RoadKey> prune_dangling(const std::vector<RoadKey>& keys,
                                    std::size_t pointCount) {
    std::vector<std::vector<std::uint32_t>> incidence(pointCount);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        incidence[keys[i].lo].push_back(static_cast<std::uint32_t>(i));
        incidence[keys[i].hi].push_back(static_cast<std::uint32_t>(i));
    }
    std::vector<bool> alive(keys.size(), true);
    std::vector<std::size_t> degree(pointCount);
    for (std::size_t v = 0; v < pointCount; ++v) {
        degree[v] = incidence[v].size();
    }
    std::vector<std::uint32_t> queue;
    for (std::size_t v = 0; v < pointCount; ++v) {
        if (degree[v] == 1) {
            queue.push_back(static_cast<std::uint32_t>(v));
        }
    }
    std::size_t head = 0;
    while (head < queue.size()) {
        const std::uint32_t v = queue[head++];
        if (degree[v] != 1) {
            continue;
        }
        for (const std::uint32_t ei : incidence[v]) {
            if (!alive[ei]) {
                continue;
            }
            alive[ei] = false;
            const RoadKey& key = keys[ei];
            const std::uint32_t other =
                (key.lo == v) ? key.hi : key.lo;
            --degree[v];
            if (other < pointCount) {
                --degree[other];
                if (degree[other] == 1) {
                    queue.push_back(other);
                }
            }
            break;
        }
    }
    std::vector<RoadKey> out;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (alive[i]) {
            out.push_back(keys[i]);
        }
    }
    return out;
}

double point_distance(const ParcelPoint& a, const ParcelPoint& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

// ---- ParcelPolygon helpers -----------------------------------------------

double ParcelPolygon::ring_area(const std::vector<ParcelPoint>& ring) {
    return ring_area_impl(ring);
}

double ParcelPolygon::area() const {
    double a = ring_area_impl(outer);
    for (const auto& hole : holes) {
        a -= std::abs(ring_area_impl(hole));
    }
    return a;
}

bool ParcelPolygon::contains(const ParcelPoint& p) const {
    auto inside_ring = [&p](const std::vector<ParcelPoint>& ring) {
        bool inside = false;
        for (std::size_t i = 0, j = ring.size() - 1; i < ring.size();
             j = i++) {
            const ParcelPoint& a = ring[i];
            const ParcelPoint& b = ring[j];
            const bool crosses =
                ((a.y > p.y) != (b.y > p.y)) &&
                (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x);
            if (crosses) {
                inside = !inside;
            }
        }
        return inside;
    };
    if (!inside_ring(outer)) {
        return false;
    }
    for (const auto& hole : holes) {
        if (inside_ring(hole)) {
            return false;
        }
    }
    return true;
}

// ---- Road network builder ------------------------------------------------

class RoadNetworkBuilder final : public IRoadNetworkBuilder {
public:
    bool build(const RoadNetworkSpec& spec, std::string& errorOut) override {
        if (spec.points.size() < 3) {
            errorOut = "road network: need at least 3 junction points";
            return false;
        }
        if (has_duplicate_points(spec.points)) {
            errorOut = "road network: duplicate junction points are not "
                       "supported";
            return false;
        }
        std::vector<double> coords;
        coords.reserve(spec.points.size() * 2);
        for (const ParcelPoint& p : spec.points) {
            coords.push_back(p.x);
            coords.push_back(p.y);
        }
        delaunator::Delaunator d(coords);
        if (d.triangles.size() < 3) {
            errorOut = "road network: no triangles (collinear/degenerate "
                       "point set)";
            return false;
        }
        RoadNetwork network;
        network.points = spec.points;
        // Undirected edges: hull halfedges (twin INVALID) and interior
        // halfedges once (e < twin).
        struct Candidate {
            double length;
            std::uint32_t a;
            std::uint32_t b;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(d.halfedges.size());
        for (std::size_t e = 0; e < d.halfedges.size(); ++e) {
            const std::size_t twin = d.halfedges[e];
            if (twin != kInvalidIndex && e >= twin) {
                continue;
            }
            const std::size_t a = d.triangles[e];
            const std::size_t b = d.triangles[next_halfedge(e)];
            candidates.push_back(
                { point_distance(spec.points[a], spec.points[b]),
                  static_cast<std::uint32_t>(a),
                  static_cast<std::uint32_t>(b) });
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& x, const Candidate& y) {
                      if (x.length != y.length) {
                          return x.length < y.length;
                      }
                      if (std::min(x.a, x.b) != std::min(y.a, y.b)) {
                          return std::min(x.a, x.b) < std::min(y.a, y.b);
                      }
                      return std::max(x.a, x.b) < std::max(y.a, y.b);
                  });
        for (const Candidate& c : candidates) {
            if (spec.maxEdgeLength > 0.0 && c.length > spec.maxEdgeLength) {
                break;  // sorted by length: all remaining are longer
            }
            network.edges.push_back({ c.a, c.b });
        }
        spec_ = spec;
        network_ = std::move(network);
        return true;
    }

    const RoadNetwork& network() const override { return network_; }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss << "{\"version\":1,\"maxEdgeLength\":"
           << format_number(spec_.maxEdgeLength) << ",\"points\":[";
        for (std::size_t i = 0; i < spec_.points.size(); ++i) {
            if (i > 0) {
                ss << ',';
            }
            ss << '[' << format_number(spec_.points[i].x) << ','
               << format_number(spec_.points[i].y) << ']';
        }
        ss << "]}";
        out = ss.str();
        return true;
    }

    bool deserialize(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue document;
        if (!sdk::json_parse(json, document, errorOut)) {
            errorOut = "road network: malformed asset - " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "road network: asset must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "road network: unsupported asset version";
            return false;
        }
        const sdk::JsonValue* points = document.field("points");
        if (points == nullptr || !points->is_array()) {
            errorOut = "road network: asset has no \"points\" array";
            return false;
        }
        RoadNetworkSpec parsed;
        parsed.maxEdgeLength =
            sdk::json_number(document, "maxEdgeLength", 0.0);
        if (parsed.maxEdgeLength < 0.0) {
            errorOut = "road network: maxEdgeLength must be non-negative";
            return false;
        }
        for (std::size_t i = 0; i < points->array.size(); ++i) {
            const sdk::JsonValue& entry = points->array[i];
            if (!entry.is_array() || entry.array.size() != 2 ||
                entry.array[0].kind != sdk::JsonValue::Kind::Number ||
                entry.array[1].kind != sdk::JsonValue::Kind::Number) {
                errorOut = "road network: point " + std::to_string(i) +
                           " must be [x, y]";
                return false;
            }
            parsed.points.push_back(
                { entry.array[0].number, entry.array[1].number });
        }
        if (parsed.points.size() < 3) {
            errorOut = "road network: need at least 3 junction points";
            return false;
        }
        // All-or-nothing: only commit once the spec builds cleanly.
        RoadNetworkBuilder rebuilt;
        std::string buildError;
        if (!rebuilt.build(parsed, buildError)) {
            errorOut = "road network: " + buildError;
            return false;
        }
        spec_ = parsed;
        network_ = rebuilt.network_;
        return true;
    }

private:
    static std::string format_number(double v) {
        std::ostringstream ss;
        ss.precision(17);
        ss << v;
        return ss.str();
    }

    RoadNetworkSpec spec_;
    RoadNetwork network_;
};

// ---- Parcellation --------------------------------------------------------

class Parcellation final : public IParcellation {
public:
    bool parcels_from_network(const RoadNetwork& network,
                              std::vector<ParcelPolygon>& out,
                              std::string& errorOut) override {
        out.clear();
        if (network.points.size() < 3) {
            errorOut = "parcellation: need at least 3 points";
            return false;
        }
        if (has_duplicate_points(network.points)) {
            errorOut = "parcellation: duplicate points are not supported";
            return false;
        }
        std::vector<double> coords;
        coords.reserve(network.points.size() * 2);
        for (const ParcelPoint& p : network.points) {
            coords.push_back(p.x);
            coords.push_back(p.y);
        }
        delaunator::Delaunator d(coords);
        if (d.triangles.size() < 3) {
            errorOut = "parcellation: no triangles (collinear/degenerate "
                       "point set)";
            return false;
        }

        std::vector<RoadKey> roadKeys =
            prune_dangling(make_road_keys(network.edges), network.points.size());
        const std::size_t triangleCount = d.triangles.size() / 3;
        const std::size_t halfedgeCount = d.halfedges.size();

        auto triangle_of = [](std::size_t e) { return e / 3; };
        auto is_hull = [&](std::size_t e) {
            return d.halfedges[e] == kInvalidIndex;
        };
        auto is_road = [&](std::size_t e) {
            return has_road(roadKeys, static_cast<std::uint32_t>(d.triangles[e]),
                            static_cast<std::uint32_t>(
                                d.triangles[next_halfedge(e)]));
        };

        // Union-find over triangles: merge across shared non-road edges.
        std::vector<std::size_t> parent(triangleCount);
        for (std::size_t i = 0; i < triangleCount; ++i) {
            parent[i] = i;
        }
        auto find = [&](std::size_t x) {
            while (parent[x] != x) {
                parent[x] = parent[parent[x]];
                x = parent[x];
            }
            return x;
        };
        auto unite = [&](std::size_t a, std::size_t b) {
            const std::size_t ra = find(a);
            const std::size_t rb = find(b);
            if (ra != rb) {
                parent[ra] = rb;
            }
        };
        for (std::size_t e = 0; e < halfedgeCount; ++e) {
            const std::size_t twin = d.halfedges[e];
            if (twin == kInvalidIndex || is_road(e)) {
                continue;
            }
            unite(triangle_of(e), triangle_of(twin));
        }

        // Group triangles by component, preserving first-seen order.
        std::unordered_map<std::size_t, std::size_t> componentOf;
        std::vector<std::vector<std::size_t>> components;
        for (std::size_t t = 0; t < triangleCount; ++t) {
            const std::size_t root = find(t);
            auto it = componentOf.find(root);
            if (it == componentOf.end()) {
                componentOf[root] = components.size();
                components.emplace_back();
                it = componentOf.find(root);
            }
            components[it->second].push_back(t);
        }

        // Per-component membership set for border checks.
        std::vector<std::vector<bool>> compHasTriangle(components.size());
        for (std::size_t c = 0; c < components.size(); ++c) {
            compHasTriangle[c].assign(triangleCount, false);
            for (const std::size_t t : components[c]) {
                compHasTriangle[c][t] = true;
            }
        }

        std::vector<bool> visited(halfedgeCount, false);
        for (std::size_t c = 0; c < components.size(); ++c) {
            // Border halfedges of this component: road or hull edges whose
            // triangle belongs to the component.
            std::vector<std::size_t> border;
            for (const std::size_t t : components[c]) {
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::size_t e = t * 3 + k;
                    if (is_road(e) || is_hull(e)) {
                        border.push_back(e);
                    }
                }
            }
            if (border.empty()) {
                continue;
            }
            std::vector<std::vector<ParcelPoint>> rings;
            for (const std::size_t start : border) {
                if (visited[start]) {
                    continue;
                }
                std::vector<ParcelPoint> ring;
                std::size_t e = start;
                do {
                    visited[e] = true;
                    ring.push_back(network.points[d.triangles[e]]);
                    // Next halfedge of the same face: at the target vertex,
                    // the next road/hull halfedge after this one's reverse.
                    std::size_t n = next_halfedge(e);
                    while (!is_road(n) && !is_hull(n)) {
                        n = next_halfedge(d.halfedges[n]);
                    }
                    e = n;
                } while (e != start);
                const double area = ring_area_impl(ring);
                if (std::abs(area) > kAreaEpsilon) {
                    rings.push_back(std::move(ring));
                }
            }
            if (rings.empty()) {
                continue;
            }
            // Outer = ring with the largest |area|; holes = the rest.
            std::size_t outerIdx = 0;
            for (std::size_t i = 1; i < rings.size(); ++i) {
                if (std::abs(ring_area_impl(rings[i])) >
                    std::abs(ring_area_impl(rings[outerIdx]))) {
                    outerIdx = i;
                }
            }
            ParcelPolygon parcel;
            parcel.outer = std::move(rings[outerIdx]);
            for (std::size_t i = 0; i < rings.size(); ++i) {
                if (i == outerIdx) {
                    continue;
                }
                parcel.holes.push_back(std::move(rings[i]));
            }
            // Normalize: outer CCW (positive), holes CW (negative).
            if (ring_area_impl(parcel.outer) < 0.0) {
                std::reverse(parcel.outer.begin(), parcel.outer.end());
            }
            for (auto& hole : parcel.holes) {
                if (ring_area_impl(hole) > 0.0) {
                    std::reverse(hole.begin(), hole.end());
                }
            }
            out.push_back(std::move(parcel));
        }
        return true;
    }

    bool triangulate(const ParcelPolygon& parcel,
                     std::vector<std::uint32_t>& out,
                     std::string& errorOut) override {
        out.clear();
        if (parcel.outer.size() < 3) {
            errorOut = "parcellation: parcel outer ring needs at least 3 "
                       "points";
            return false;
        }
        std::vector<std::vector<std::array<double, 2>>> rings;
        rings.reserve(1 + parcel.holes.size());
        auto to_array = [](const std::vector<ParcelPoint>& ring) {
            std::vector<std::array<double, 2>> arr;
            arr.reserve(ring.size());
            for (const ParcelPoint& p : ring) {
                arr.push_back({ p.x, p.y });
            }
            return arr;
        };
        rings.push_back(to_array(parcel.outer));
        for (const auto& hole : parcel.holes) {
            if (hole.size() < 3) {
                errorOut = "parcellation: parcel hole ring needs at least 3 "
                           "points";
                return false;
            }
            rings.push_back(to_array(hole));
        }
        std::vector<std::uint32_t> indices =
            mapbox::earcut<std::uint32_t>(rings);
        if (indices.empty()) {
            errorOut = "parcellation: earcut produced no triangles "
                       "(degenerate polygon)";
            return false;
        }
        out = std::move(indices);
        return true;
    }
};

}  // namespace procgen
}  // namespace engine

namespace engine {
namespace procgen {

std::shared_ptr<IRoadNetworkBuilder> create_road_network_builder() {
    return std::make_shared<RoadNetworkBuilder>();
}

std::shared_ptr<IParcellation> create_parcellation() {
    return std::make_shared<Parcellation>();
}

}  // namespace procgen
}  // namespace engine
