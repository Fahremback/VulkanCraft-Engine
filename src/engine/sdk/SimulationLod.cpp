// SimulationLod.cpp — the only TU implementing the public Simulation LOD
// contract (FALTANTES §20). The runtime is PURE and DETERMINISTIC: it never
// touches a concrete world — it turns a (focus, dt) sequence plus the
// caller-owned SimulationLodState into tier decisions, region ticks, sleeping,
// deterministic aggregate evolution and scheduled-event firings, all reported
// as SimulationLodEvents + a dirty-region set the project applies. Same spec
// + same (focus, dt) sequence -> identical state and event streams, bit-exact,
// across instances.
//
// Design: regions are grid cells aligned to the origin (the IMultiScaleStreaming
// convention); each update re-tiers every region by its distance to the focus,
// ticks regions whose tier interval elapsed, sleeps/wakes regions crossing the
// Sleeping tier, evolves Aggregate regions through a deterministic seasonal
// model (splitmix64 per-region seed + the world clock's season), fires due
// scheduled events and enforces per-tier region budgets (overflow falls to the
// next cheaper tier). The whole state serializes bit-exactly (%.9g) for
// save/load and replication.
//
// Numeric validation uses BIT-LEVEL finite checks: the project compiles with
// /fp:fast (findings #79), which folds std::isfinite(NaN) to true.

#include "engine/simulation/ISimulationLod.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>

namespace engine {
namespace simulation {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

const char* mode_name(SimulationLodMode mode) {
    switch (mode) {
        case SimulationLodMode::Full: return "full";
        case SimulationLodMode::Coarse: return "coarse";
        case SimulationLodMode::Aggregate: return "aggregate";
        case SimulationLodMode::Sleeping: break;
    }
    return "sleeping";
}

bool parse_mode(const std::string& name, SimulationLodMode& out) {
    if (name == "full") { out = SimulationLodMode::Full; return true; }
    if (name == "coarse") { out = SimulationLodMode::Coarse; return true; }
    if (name == "aggregate") { out = SimulationLodMode::Aggregate; return true; }
    if (name == "sleeping") { out = SimulationLodMode::Sleeping; return true; }
    return false;
}

// ---- deterministic hashing (splitmix64, integer arithmetic only) ------------

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Deterministic [0, 1) from a hash (24 bits — enough for climate noise).
float hash01(std::uint64_t h) {
    return static_cast<float>(h & 0x00ffffffull) / static_cast<float>(0x01000000ull);
}

std::uint64_t region_seed(std::int64_t cellX, std::int64_t cellZ, int season) {
    std::uint64_t h = 0x9e3779b97f4a7c15ull;
    h = splitmix64(h ^ static_cast<std::uint64_t>(cellX));
    h = splitmix64(h ^ static_cast<std::uint64_t>(cellZ));
    h = splitmix64(h ^ static_cast<std::uint64_t>(static_cast<std::int32_t>(season)));
    return h;
}

// ---- validation --------------------------------------------------------------

bool validate_spec(const SimulationLodSpec& spec, std::string& errorOut) {
    auto fail = [&](const std::string& message) {
        errorOut = "simulation lod spec: " + message;
        return false;
    };
    if (spec.version != 1) return fail("unsupported version");
    if (!finite_float(spec.cellSize) || spec.cellSize <= 0.0f) {
        return fail("cellSize must be finite and > 0");
    }
    if (!finite_float(spec.fullRadius) || spec.fullRadius < 0.0f) {
        return fail("fullRadius must be finite and >= 0");
    }
    if (!finite_float(spec.falloffRadius) || !(spec.falloffRadius > spec.fullRadius)) {
        return fail("falloffRadius must be finite and > fullRadius");
    }
    if (!finite_float(spec.dayLengthSeconds) || !(spec.dayLengthSeconds > 0.0f)) {
        return fail("dayLengthSeconds must be finite and > 0");
    }
    if (spec.daysPerSeason < 1) return fail("daysPerSeason must be >= 1");
    if (spec.tiers.empty()) return fail("at least one tier is required");
    std::set<std::string> names;
    float previous = 2.0f;
    for (const SimulationLodTier& tier : spec.tiers) {
        if (tier.name.empty()) return fail("tier name must not be empty");
        if (!names.insert(tier.name).second) return fail("duplicate tier name '" + tier.name + "'");
        if (!finite_float(tier.minRelevance) || tier.minRelevance < 0.0f ||
            tier.minRelevance > 1.0f) {
            return fail("tier '" + tier.name + "' minRelevance must be in [0, 1]");
        }
        if (!(tier.minRelevance < previous)) {
            return fail("tiers must be sorted by minRelevance descending (tier '" +
                        tier.name + "')");
        }
        previous = tier.minRelevance;
        if (!finite_float(tier.updateInterval) || tier.updateInterval < 0.0f) {
            return fail("tier '" + tier.name + "' updateInterval must be finite and >= 0");
        }
        if (!finite_float(tier.sleepAfterIdle) || tier.sleepAfterIdle < 0.0f) {
            return fail("tier '" + tier.name + "' sleepAfterIdle must be finite and >= 0");
        }
        if (tier.maxRegions < 0) return fail("tier '" + tier.name + "' maxRegions must be >= 0");
        if (!finite_float(tier.aggregateInterval) || tier.aggregateInterval < 0.0f) {
            return fail("tier '" + tier.name + "' aggregateInterval must be finite and >= 0");
        }
    }
    return true;
}

bool validate_state(const SimulationLodState& state, std::string& errorOut) {
    if (state.version != 1) {
        errorOut = "simulation lod state: unsupported version";
        return false;
    }
    for (const SimulationRegionState& region : state.regions) {
        if (!finite_float(region.idleSeconds) || !finite_float(region.elapsedSinceTick) ||
            !finite_float(region.aggregateAccumulator) || !finite_float(region.population) ||
            !finite_float(region.resources)) {
            errorOut = "simulation lod state: region has a non-finite field";
            return false;
        }
    }
    if (!finite_float(state.clock.totalSeconds) || !finite_float(state.clock.timeOfDay)) {
        errorOut = "simulation lod state: clock has a non-finite field";
        return false;
    }
    return true;
}

// ---- emitters ----------------------------------------------------------------

std::string emit_tier(const SimulationLodTier& tier) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"name\":\"" << json_escape(tier.name) << "\",\"mode\":\""
        << mode_name(tier.mode) << "\",\"minRelevance\":" << tier.minRelevance
        << ",\"updateInterval\":" << tier.updateInterval
        << ",\"sleepAfterIdle\":" << tier.sleepAfterIdle
        << ",\"maxRegions\":" << tier.maxRegions
        << ",\"aggregateInterval\":" << tier.aggregateInterval << '}';
    return out.str();
}

std::string emit_spec(const SimulationLodSpec& spec) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":" << spec.version
        << ",\"cellSize\":" << spec.cellSize
        << ",\"fullRadius\":" << spec.fullRadius
        << ",\"falloffRadius\":" << spec.falloffRadius
        << ",\"dayLengthSeconds\":" << spec.dayLengthSeconds
        << ",\"daysPerSeason\":" << spec.daysPerSeason
        << ",\"tiers\":[";
    for (std::size_t i = 0; i < spec.tiers.size(); ++i) {
        if (i != 0) out << ',';
        out << emit_tier(spec.tiers[i]);
    }
    out << "]}";
    return out.str();
}

std::string emit_region(const SimulationRegionState& region) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"cellX\":" << region.cellX << ",\"cellZ\":" << region.cellZ
        << ",\"tier\":" << region.tier
        << ",\"idleSeconds\":" << region.idleSeconds
        << ",\"elapsedSinceTick\":" << region.elapsedSinceTick
        << ",\"aggregateAccumulator\":" << region.aggregateAccumulator
        << ",\"population\":" << region.population
        << ",\"resources\":" << region.resources
        << ",\"tags\":[";
    for (std::size_t i = 0; i < region.tags.size(); ++i) {
        if (i != 0) out << ',';
        out << '"' << json_escape(region.tags[i]) << '"';
    }
    out << "]}";
    return out.str();
}

std::string emit_clock(const SimulationWorldClock& clock) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"tick\":" << clock.tick
        << ",\"totalSeconds\":" << clock.totalSeconds
        << ",\"timeOfDay\":" << clock.timeOfDay
        << ",\"day\":" << clock.day
        << ",\"season\":" << clock.season << '}';
    return out.str();
}

std::string emit_scheduled_event(const SimulationScheduledEvent& event) {
    std::ostringstream out;
    out << "{\"tickIndex\":" << event.tickIndex
        << ",\"cellX\":" << event.cellX
        << ",\"cellZ\":" << event.cellZ
        << ",\"payload\":\"" << json_escape(event.payload) << '"'
        << ",\"fired\":" << (event.fired ? "true" : "false") << '}';
    return out.str();
}

// ---- readers -----------------------------------------------------------------

bool read_tier(const sdk::JsonValue& value, SimulationLodTier& out, std::string& errorOut) {
    if (!value.is_object()) {
        errorOut = "simulation lod spec: each tier must be an object";
        return false;
    }
    SimulationLodTier tier;
    tier.name = sdk::json_string(value, "name", "");
    const std::string mode = sdk::json_string(value, "mode", "full");
    if (!parse_mode(mode, tier.mode)) {
        errorOut = "simulation lod spec: unknown tier mode '" + mode + "'";
        return false;
    }
    tier.minRelevance = static_cast<float>(sdk::json_number(value, "minRelevance", 0.0));
    tier.updateInterval = static_cast<float>(sdk::json_number(value, "updateInterval", 0.0));
    tier.sleepAfterIdle = static_cast<float>(sdk::json_number(value, "sleepAfterIdle", 0.0));
    tier.maxRegions = static_cast<int>(sdk::json_number(value, "maxRegions", 0.0));
    tier.aggregateInterval = static_cast<float>(sdk::json_number(value, "aggregateInterval", 0.0));
    out = std::move(tier);
    return true;
}

bool read_region(const sdk::JsonValue& value, SimulationRegionState& out, std::string& errorOut) {
    if (!value.is_object()) {
        errorOut = "simulation lod state: each region must be an object";
        return false;
    }
    SimulationRegionState region;
    region.cellX = static_cast<std::int64_t>(sdk::json_number(value, "cellX", 0.0));
    region.cellZ = static_cast<std::int64_t>(sdk::json_number(value, "cellZ", 0.0));
    region.tier = static_cast<std::size_t>(sdk::json_number(value, "tier", 0.0));
    region.idleSeconds = static_cast<float>(sdk::json_number(value, "idleSeconds", 0.0));
    region.elapsedSinceTick = static_cast<float>(sdk::json_number(value, "elapsedSinceTick", 0.0));
    region.aggregateAccumulator = static_cast<float>(sdk::json_number(value, "aggregateAccumulator", 0.0));
    region.population = static_cast<float>(sdk::json_number(value, "population", 0.0));
    region.resources = static_cast<float>(sdk::json_number(value, "resources", 0.0));
    region.tags = sdk::json_string_array(value, "tags");
    out = std::move(region);
    return true;
}

bool read_scheduled_event(const sdk::JsonValue& value, SimulationScheduledEvent& out,
                          std::string& errorOut) {
    if (!value.is_object()) {
        errorOut = "simulation lod state: each scheduled event must be an object";
        return false;
    }
    SimulationScheduledEvent event;
    event.tickIndex = static_cast<std::uint64_t>(sdk::json_number(value, "tickIndex", 0.0));
    event.cellX = static_cast<std::int64_t>(sdk::json_number(value, "cellX", 0.0));
    event.cellZ = static_cast<std::int64_t>(sdk::json_number(value, "cellZ", 0.0));
    event.payload = sdk::json_string(value, "payload", "");
    event.fired = sdk::json_bool(value, "fired", false);
    out = std::move(event);
    return true;
}

// ---- the runtime --------------------------------------------------------------

class SimulationLod final : public ISimulationLod {
public:
    bool set_spec(const SimulationLodSpec& spec, std::string& errorOut) override {
        if (!validate_spec(spec, errorOut)) return false;
        spec_ = spec;
        hasSpec_ = true;
        return true;
    }

    const SimulationLodSpec* spec() const override {
        return hasSpec_ ? &spec_ : nullptr;
    }

    bool select_tier(float relevance, std::size_t& tierIndexOut,
                     std::string& errorOut) const override {
        if (!hasSpec_) {
            errorOut = "simulation lod: no spec set";
            return false;
        }
        if (!finite_float(relevance) || relevance < 0.0f || relevance > 1.0f) {
            errorOut = "simulation lod: relevance must be in [0, 1]";
            return false;
        }
        for (std::size_t i = 0; i < spec_.tiers.size(); ++i) {
            if (relevance >= spec_.tiers[i].minRelevance) {
                tierIndexOut = i;
                return true;
            }
        }
        tierIndexOut = spec_.tiers.size() - 1;
        return true;
    }

    void region_cell(float worldX, float worldZ, std::int64_t& cellX,
                     std::int64_t& cellZ) const override {
        cellX = static_cast<std::int64_t>(std::floor(worldX / spec_.cellSize));
        cellZ = static_cast<std::int64_t>(std::floor(worldZ / spec_.cellSize));
    }

    float relevance(float distance) const override {
        if (distance <= spec_.fullRadius) return 1.0f;
        if (distance >= spec_.falloffRadius) return 0.0f;
        return (spec_.falloffRadius - distance) /
               (spec_.falloffRadius - spec_.fullRadius);
    }

    bool region_key(std::int64_t cellX, std::int64_t cellZ, std::string& out) const override {
        std::ostringstream stream;
        stream << cellX << ',' << cellZ;
        out = stream.str();
        return true;
    }

    bool add_region(SimulationLodState& state, std::int64_t cellX, std::int64_t cellZ,
                    const std::vector<std::string>& tags,
                    std::string& errorOut) override {
        if (!validate_state(state, errorOut)) return false;
        for (const SimulationRegionState& existing : state.regions) {
            if (existing.cellX == cellX && existing.cellZ == cellZ) {
                errorOut = "simulation lod: region '" + key_of(cellX, cellZ) + "' already exists";
                return false;
            }
        }
        SimulationRegionState region;
        region.cellX = cellX;
        region.cellZ = cellZ;
        region.tags = tags;
        region.tier = 0;
        state.regions.push_back(std::move(region));
        return true;
    }

    bool remove_region(SimulationLodState& state, std::int64_t cellX,
                       std::int64_t cellZ) override {
        bool found = false;
        for (auto it = state.regions.begin(); it != state.regions.end();) {
            if (it->cellX == cellX && it->cellZ == cellZ) {
                it = state.regions.erase(it);
                found = true;
            } else {
                ++it;
            }
        }
        if (found) {
            for (auto it = state.scheduledEvents.begin(); it != state.scheduledEvents.end();) {
                if (it->cellX == cellX && it->cellZ == cellZ) {
                    it = state.scheduledEvents.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return found;
    }

    bool schedule_event(SimulationLodState& state, std::uint64_t tickIndex,
                        std::int64_t cellX, std::int64_t cellZ,
                        const std::string& payload, std::string& errorOut) override {
        if (!validate_state(state, errorOut)) return false;
        if (tickIndex < state.clock.tick) {
            errorOut = "simulation lod: cannot schedule an event in the past";
            return false;
        }
        if (payload.empty()) {
            errorOut = "simulation lod: event payload must not be empty";
            return false;
        }
        SimulationScheduledEvent event;
        event.tickIndex = tickIndex;
        event.cellX = cellX;
        event.cellZ = cellZ;
        event.payload = payload;
        state.scheduledEvents.push_back(std::move(event));
        return true;
    }

    bool update(SimulationLodState& state, float focusX, float focusZ, float dtSeconds,
                std::vector<SimulationLodEvent>& events,
                std::string& errorOut) override {
        if (!hasSpec_) {
            errorOut = "simulation lod: no spec set";
            return false;
        }
        if (!validate_state(state, errorOut)) return false;
        if (!finite_float(dtSeconds) || dtSeconds < 0.0f) {
            errorOut = "simulation lod: dt must be finite and >= 0";
            return false;
        }
        for (const SimulationRegionState& region : state.regions) {
            if (region.tier >= spec_.tiers.size()) {
                errorOut = "simulation lod: state region has a tier outside the spec";
                return false;
            }
        }
        events.clear();
        state.dirtyRegionKeys.clear();

        // 1. advance the world clock (tick/totalSeconds -> timeOfDay/day/season).
        state.clock.tick += 1;
        state.clock.totalSeconds += dtSeconds;
        const float dayFraction = state.clock.totalSeconds / spec_.dayLengthSeconds;
        state.clock.day = static_cast<std::uint64_t>(std::floor(dayFraction));
        state.clock.timeOfDay = dayFraction - static_cast<float>(state.clock.day);
        state.clock.season = static_cast<int>(
            (state.clock.day / static_cast<std::uint64_t>(spec_.daysPerSeason)) % 4);

        // 2. re-tier and tick regions. Deterministic order: (cellX, cellZ).
        std::sort(state.regions.begin(), state.regions.end(),
                  [](const SimulationRegionState& a, const SimulationRegionState& b) {
                      if (a.cellX != b.cellX) return a.cellX < b.cellX;
                      return a.cellZ < b.cellZ;
                  });

        for (SimulationRegionState& region : state.regions) {
            const float centerX = (static_cast<float>(region.cellX) + 0.5f) * spec_.cellSize;
            const float centerZ = (static_cast<float>(region.cellZ) + 0.5f) * spec_.cellSize;
            const float dx = centerX - focusX;
            const float dz = centerZ - focusZ;
            const float distance = std::sqrt(dx * dx + dz * dz);
            const float rel = relevance(distance);
            std::size_t selected = 0;
            std::string selectionError;
            select_tier(rel, selected, selectionError);

            if (selected != region.tier) {
                const SimulationLodTier& oldTier = spec_.tiers[region.tier];
                const SimulationLodTier& newTier = spec_.tiers[selected];
                const bool wasSleeping = oldTier.mode == SimulationLodMode::Sleeping;
                const bool nowSleeping = newTier.mode == SimulationLodMode::Sleeping;
                SimulationLodEvent event;
                event.cellX = region.cellX;
                event.cellZ = region.cellZ;
                event.fromTier = region.tier;
                event.toTier = selected;
                if (nowSleeping && !wasSleeping) {
                    region.idleSeconds = 0.0f;
                    event.kind = SimulationLodEvent::Kind::RegionSlept;
                } else if (wasSleeping && !nowSleeping) {
                    region.idleSeconds = 0.0f;
                    event.kind = SimulationLodEvent::Kind::RegionWoken;
                } else {
                    event.kind = SimulationLodEvent::Kind::RegionTierChanged;
                    event.population = region.population;
                    event.resources = region.resources;
                }
                region.tier = selected;
                events.push_back(std::move(event));
                mark_dirty(state, region.cellX, region.cellZ);
            }

            const SimulationLodTier& tier = spec_.tiers[region.tier];
            if (tier.mode == SimulationLodMode::Sleeping) {
                region.idleSeconds += dtSeconds;
                region.elapsedSinceTick = 0.0f;
                continue;
            }

            // 3. frequency gate: the interval IS the LOD.
            region.elapsedSinceTick += dtSeconds;
            const bool everyTick = tier.updateInterval <= 0.0f;
            if (!everyTick && region.elapsedSinceTick < tier.updateInterval) {
                continue;
            }

            if (tier.mode == SimulationLodMode::Aggregate) {
                region.aggregateAccumulator += dtSeconds;
                const bool aggregateEvery = tier.aggregateInterval <= 0.0f;
                if (!aggregateEvery && region.aggregateAccumulator < tier.aggregateInterval) {
                    continue;
                }
                SimulationClimate climate;
                climate_impl(state, region, climate);
                evolve_aggregate(region, dtSeconds, climate.growth);
                SimulationLodEvent event;
                event.kind = SimulationLodEvent::Kind::AggregateUpdate;
                event.cellX = region.cellX;
                event.cellZ = region.cellZ;
                event.population = region.population;
                event.resources = region.resources;
                events.push_back(std::move(event));
                region.aggregateAccumulator = 0.0f;
                mark_dirty(state, region.cellX, region.cellZ);
            } else {
                SimulationLodEvent event;
                event.kind = SimulationLodEvent::Kind::RegionTick;
                event.cellX = region.cellX;
                event.cellZ = region.cellZ;
                events.push_back(std::move(event));
                mark_dirty(state, region.cellX, region.cellZ);
            }
            region.elapsedSinceTick = 0.0f;
        }

        // 4. budgets: per-tier maxRegions; the FARTHEST excess regions fall to
        // the next cheaper tier (deterministic: distance desc, cell asc).
        for (std::size_t tierIndex = 0; tierIndex < spec_.tiers.size(); ++tierIndex) {
            const int cap = spec_.tiers[tierIndex].maxRegions;
            if (cap <= 0) continue;
            std::vector<std::size_t> members;
            for (std::size_t i = 0; i < state.regions.size(); ++i) {
                if (state.regions[i].tier == tierIndex) members.push_back(i);
            }
            if (static_cast<int>(members.size()) <= cap) continue;
            std::sort(members.begin(), members.end(),
                      [&](std::size_t a, std::size_t b) {
                          const float distA = distance_to_focus(state.regions[a], focusX, focusZ);
                          const float distB = distance_to_focus(state.regions[b], focusX, focusZ);
                          if (distA != distB) return distA > distB;
                          if (state.regions[a].cellX != state.regions[b].cellX) {
                              return state.regions[a].cellX < state.regions[b].cellX;
                          }
                          return state.regions[a].cellZ < state.regions[b].cellZ;
                      });
            const std::size_t excess = members.size() - static_cast<std::size_t>(cap);
            for (std::size_t k = 0; k < excess; ++k) {
                SimulationRegionState& region = state.regions[members[k]];
                const std::size_t fallback =
                    std::min(tierIndex + 1, spec_.tiers.size() - 1);
                if (fallback == region.tier) continue;
                SimulationLodEvent event;
                event.kind = SimulationLodEvent::Kind::RegionTierChanged;
                event.cellX = region.cellX;
                event.cellZ = region.cellZ;
                event.fromTier = region.tier;
                event.toTier = fallback;
                event.population = region.population;
                event.resources = region.resources;
                region.tier = fallback;
                events.push_back(std::move(event));
                mark_dirty(state, region.cellX, region.cellZ);
            }
        }

        // 5. scheduled events: fire at the exact tick (persisted; fired kept).
        for (SimulationScheduledEvent& scheduled : state.scheduledEvents) {
            if (scheduled.fired) continue;
            if (scheduled.tickIndex == state.clock.tick) {
                scheduled.fired = true;
                SimulationLodEvent event;
                event.kind = SimulationLodEvent::Kind::EventDue;
                event.cellX = scheduled.cellX;
                event.cellZ = scheduled.cellZ;
                event.payload = scheduled.payload;
                events.push_back(std::move(event));
                mark_dirty(state, scheduled.cellX, scheduled.cellZ);
            }
        }
        return true;
    }

    bool climate(const SimulationLodState& state, const SimulationRegionState& region,
                 SimulationClimate& out) const override {
        if (!hasSpec_) return false;
        climate_impl(state, region, out);
        return true;
    }

    bool serialize_state(const SimulationLodState& state, std::string& out,
                         std::string& errorOut) const override {
        if (!validate_state(state, errorOut)) return false;
        std::ostringstream stream;
        stream << std::setprecision(9);
        stream << "{\"version\":" << state.version
               << ",\"clock\":" << emit_clock(state.clock)
               << ",\"regions\":[";
        for (std::size_t i = 0; i < state.regions.size(); ++i) {
            if (i != 0) stream << ',';
            stream << emit_region(state.regions[i]);
        }
        stream << "],\"scheduledEvents\":[";
        for (std::size_t i = 0; i < state.scheduledEvents.size(); ++i) {
            if (i != 0) stream << ',';
            stream << emit_scheduled_event(state.scheduledEvents[i]);
        }
        stream << "]}";
        out = stream.str();
        return true;
    }

    bool deserialize_state(const std::string& data, SimulationLodState& out,
                           std::string& errorOut) const override {
        sdk::JsonValue root;
        if (!sdk::json_parse(data, root, errorOut) || !root.is_object()) {
            if (errorOut.empty()) errorOut = "simulation lod state: root must be an object";
            return false;
        }
        SimulationLodState state;
        state.version = static_cast<int>(sdk::json_number(root, "version", 1));
        const sdk::JsonValue* clockValue = root.field("clock");
        if (clockValue != nullptr) {
            if (!clockValue->is_object()) {
                errorOut = "simulation lod state: clock must be an object";
                return false;
            }
            state.clock.tick = static_cast<std::uint64_t>(
                sdk::json_number(*clockValue, "tick", 0.0));
            state.clock.totalSeconds = static_cast<float>(
                sdk::json_number(*clockValue, "totalSeconds", 0.0));
            state.clock.timeOfDay = static_cast<float>(
                sdk::json_number(*clockValue, "timeOfDay", 0.0));
            state.clock.day = static_cast<std::uint64_t>(
                sdk::json_number(*clockValue, "day", 0.0));
            state.clock.season = static_cast<int>(
                sdk::json_number(*clockValue, "season", 0.0));
        }
        const sdk::JsonValue* regionsValue = root.field("regions");
        if (regionsValue != nullptr) {
            if (regionsValue->kind != sdk::JsonValue::Kind::Array) {
                errorOut = "simulation lod state: regions must be an array";
                return false;
            }
            state.regions.reserve(regionsValue->array.size());
            for (const sdk::JsonValue& item : regionsValue->array) {
                SimulationRegionState region;
                if (!read_region(item, region, errorOut)) return false;
                state.regions.push_back(std::move(region));
            }
        }
        const sdk::JsonValue* eventsValue = root.field("scheduledEvents");
        if (eventsValue != nullptr) {
            if (eventsValue->kind != sdk::JsonValue::Kind::Array) {
                errorOut = "simulation lod state: scheduledEvents must be an array";
                return false;
            }
            state.scheduledEvents.reserve(eventsValue->array.size());
            for (const sdk::JsonValue& item : eventsValue->array) {
                SimulationScheduledEvent event;
                if (!read_scheduled_event(item, event, errorOut)) return false;
                state.scheduledEvents.push_back(std::move(event));
            }
        }
        if (!validate_state(state, errorOut)) return false;
        out = std::move(state);
        return true;
    }

private:
    SimulationLodSpec spec_;
    bool hasSpec_{ false };

    static std::string key_of(std::int64_t cellX, std::int64_t cellZ) {
        std::ostringstream stream;
        stream << cellX << ',' << cellZ;
        return stream.str();
    }

    void mark_dirty(SimulationLodState& state, std::int64_t cellX,
                    std::int64_t cellZ) const {
        const std::string key = key_of(cellX, cellZ);
        for (const std::string& existing : state.dirtyRegionKeys) {
            if (existing == key) return;
        }
        state.dirtyRegionKeys.push_back(key);
    }

    float distance_to_focus(const SimulationRegionState& region, float focusX,
                            float focusZ) const {
        const float centerX = (static_cast<float>(region.cellX) + 0.5f) * spec_.cellSize;
        const float centerZ = (static_cast<float>(region.cellZ) + 0.5f) * spec_.cellSize;
        const float dx = centerX - focusX;
        const float dz = centerZ - focusZ;
        return std::sqrt(dx * dx + dz * dz);
    }

    static void climate_impl(const SimulationLodState& state,
                             const SimulationRegionState& region,
                             SimulationClimate& out) {
        // Deterministic seasonal base temperatures (Spring/Summer/Autumn/Winter).
        static const float kSeasonTemperature[4] = { 0.55f, 0.85f, 0.60f, 0.30f };
        const int season = static_cast<int>(state.clock.season & 3);
        float temperature = kSeasonTemperature[season];
        float moisture = 0.5f;
        for (const std::string& tag : region.tags) {
            if (tag == "arid") {
                moisture = std::min(moisture, 0.25f);
            } else if (tag == "forest") {
                moisture = std::max(moisture, 0.70f);
            } else if (tag == "swamp") {
                moisture = std::max(moisture, 0.85f);
            }
        }
        const std::uint64_t seed = region_seed(region.cellX, region.cellZ, season);
        temperature += (hash01(splitmix64(seed)) - 0.5f) * 0.4f;         // +/- 0.2
        moisture += (hash01(splitmix64(seed ^ 0xa5a5a5a5a5a5a5a5ull)) - 0.5f) * 0.2f;  // +/- 0.1
        temperature = std::max(0.0f, std::min(1.0f, temperature));
        moisture = std::max(0.0f, std::min(1.0f, moisture));
        // growth: freezing or arid regions do not grow; warm + wet grow strongly.
        float growth = (temperature - 0.25f) * moisture * 2.0f;
        growth = std::max(0.0f, std::min(1.0f, growth));
        out.temperature = temperature;
        out.moisture = moisture;
        out.growth = growth;
    }

    // Deterministic seasonal aggregate model: pure function of (counters, dt,
    // growth). Population grows by the seasonal growth; resources accrete from
    // population and decay slowly. Same inputs -> identical results, bit-exact.
    static void evolve_aggregate(SimulationRegionState& region, float dt,
                                 float growth) {
        region.population += region.population * growth * dt * 0.1f;
        region.resources += region.population * dt * 0.02f -
                            region.resources * dt * 0.005f;
    }
};

}  // namespace

// ---- spec contract methods (SDK) ----------------------------------------------

bool SimulationLodSpec::validate(std::string& errorOut) const {
    return validate_spec(*this, errorOut);
}

bool SimulationLodSpec::load_from_json(const std::string& jsonText,
                                       std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
        if (errorOut.empty()) errorOut = "simulation lod spec: root must be an object";
        return false;
    }
    SimulationLodSpec parsed;
    parsed.version = static_cast<int>(sdk::json_number(root, "version", 1));
    parsed.cellSize = static_cast<float>(sdk::json_number(root, "cellSize", 16.0));
    parsed.fullRadius = static_cast<float>(sdk::json_number(root, "fullRadius", 48.0));
    parsed.falloffRadius = static_cast<float>(sdk::json_number(root, "falloffRadius", 320.0));
    parsed.dayLengthSeconds = static_cast<float>(sdk::json_number(root, "dayLengthSeconds", 240.0));
    parsed.daysPerSeason = static_cast<int>(sdk::json_number(root, "daysPerSeason", 30.0));
    const sdk::JsonValue* tiersValue = root.field("tiers");
    if (tiersValue != nullptr) {
        if (tiersValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "simulation lod spec: tiers must be an array";
            return false;
        }
        parsed.tiers.reserve(tiersValue->array.size());
        for (const sdk::JsonValue& item : tiersValue->array) {
            SimulationLodTier tier;
            if (!read_tier(item, tier, errorOut)) return false;
            parsed.tiers.push_back(std::move(tier));
        }
    }
    if (!validate_spec(parsed, errorOut)) return false;
    *this = std::move(parsed);
    return true;
}

std::string SimulationLodSpec::to_json() const {
    return emit_spec(*this);
}

}  // namespace simulation
}  // namespace engine

// ---- factory -----------------------------------------------------------------

namespace engine {
namespace simulation {

std::unique_ptr<ISimulationLod> create_simulation_lod() {
    return std::make_unique<SimulationLod>();
}

}  // namespace simulation
}  // namespace engine
