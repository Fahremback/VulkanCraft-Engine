// StructurePlacement.cpp
//
// SDK adapter for engine/procgen/IStructurePlacement.hpp (META section 18 /
// FALTANTES item 14: "sistema data-driven de estruturas, sockets e regras de
// spawn"). The structure GENERATION is closed (IStructureGenerator /
// fast-wfc); this is the missing half: WHERE and WHEN structures appear in
// the world, data-driven, plus the SOCKETS that connect structures to each
// other.
//
// Model: a StructurePlacementSystem is a registry of StructureDefinitions
// (WFC asset + declared sockets + footprint) plus a pure placement engine.
// StructureSpawnRules gate spawns per candidate cell (biome names, surface
// height, density) and the whole decision is a PURE function of
// (rules, worldSeed, column): the candidate cell is derived from the column
// by the rule's spacing, the density hash and the per-placement seed come
// from splitmix64 mixing of (worldSeed, rule seedOffset, cell), so world
// population is deterministic per seed and never depends on call order or
// threads.
//
// Dangling data is refused, never skipped: a rule whose structureId is not a
// registered definition is a hard error with a diagnostic (both at
// set_rules/deserialize time and at placement time).

#include "engine/procgen/IStructurePlacement.hpp"
#include "RegistryJson.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace procgen {
namespace {

// ---- deterministic hashing (splitmix64) ----

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// A bijective-ish 64-bit mix of two inputs (splitmix64 of a sum-with-golden-
// ratio construction). Deterministic everywhere; the exact sequence is
// irrelevant as long as it is stable, which it is (pure integer arithmetic).
std::uint64_t mix64(std::uint64_t a, std::uint64_t b) {
    return splitmix64(a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2)));
}

// Hash of a candidate cell for a (world seed, rule): drives both the density
// gate and the per-placement seed. Negative world coordinates cast to
// uint64_t deterministically (two's complement), so negative cells hash
// distinctly from positive ones.
std::uint64_t cell_hash(std::uint32_t worldSeed, std::uint32_t seedOffset,
                        std::int64_t cellX, std::int64_t cellZ) {
    const std::uint64_t ruleMix =
        static_cast<std::uint64_t>(worldSeed) ^
        (static_cast<std::uint64_t>(seedOffset) * 0xBF58476D1CE4E5B9ULL);
    return mix64(mix64(ruleMix, static_cast<std::uint64_t>(cellX)),
                 static_cast<std::uint64_t>(cellZ));
}

// Per-placement seed derived from (world seed, rule, cell): the caller uses
// it for any further per-instance randomization so content stays stable per
// seed.
std::uint32_t placement_seed(std::uint32_t worldSeed, std::uint32_t seedOffset,
                             std::int64_t cellX, std::int64_t cellZ) {
    return static_cast<std::uint32_t>(
        splitmix64(cell_hash(worldSeed, seedOffset, cellX, cellZ)));
}

// Floor division (truncation toward -inf), used so negative world coordinates
// map into the correct candidate cell.
std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

std::int64_t cell_origin(std::int64_t v, std::int64_t spacing) {
    return floor_div(v, spacing) * spacing;
}

// Cardinal facings: 0 = +X, 1 = -X, 2 = +Z, 3 = -Z. Two sockets face each
// other when their codes XOR to 1 (opposite axis, opposite direction).
bool facings_oppose(int a, int b) { return (a ^ b) == 1; }

bool valid_facing(int facing) { return facing >= 0 && facing <= 3; }

const StructureSocket* find_socket(const StructureDefinition& def,
                                   const std::string& name) {
    for (const auto& socket : def.sockets) {
        if (socket.name == name) return &socket;
    }
    return nullptr;
}

// ---- validation helpers ----

// A structure asset is valid exactly when the structure generator factory
// accepts it (single source of truth — no duplicated validation logic).
bool valid_structure_spec(const StructureAssetSpec& spec, std::string& errorOut) {
    auto generator = create_structure_generator(spec, errorOut);
    return generator != nullptr;
}

bool valid_definition(const StructureDefinition& def, std::string& errorOut) {
    if (def.id.empty()) {
        errorOut = "structure placement: definition id must be non-empty";
        return false;
    }
    if (def.outputWidth < 0 || def.outputHeight < 0) {
        errorOut = "structure placement: output footprint must be >= 0";
        return false;
    }
    if (def.outputWidth > 0 && def.outputWidth < def.spec.patternSize) {
        errorOut = "structure placement: outputWidth smaller than the pattern "
                   "window";
        return false;
    }
    if (def.outputHeight > 0 && def.outputHeight < def.spec.patternSize) {
        errorOut = "structure placement: outputHeight smaller than the pattern "
                   "window";
        return false;
    }
    if (!valid_structure_spec(def.spec, errorOut)) return false;
    std::set<std::string> names;
    for (const auto& socket : def.sockets) {
        if (socket.name.empty()) {
            errorOut = "structure placement: socket name must be non-empty";
            return false;
        }
        if (!names.insert(socket.name).second) {
            errorOut = "structure placement: duplicate socket name '" +
                       socket.name + "' in '" + def.id + "'";
            return false;
        }
        if (!valid_facing(socket.facing)) {
            errorOut = "structure placement: socket facing must be 0..3";
            return false;
        }
    }
    return true;
}

bool valid_rule(const StructureSpawnRule& rule,
                const std::map<std::string, StructureDefinition>& defs,
                std::string& errorOut) {
    if (rule.structureId.empty()) {
        errorOut = "structure placement: rule structureId must be non-empty";
        return false;
    }
    if (defs.find(rule.structureId) == defs.end()) {
        errorOut = "structure placement: rule references unknown structureId '" +
                   rule.structureId + "'";
        return false;
    }
    if (rule.minSurfaceHeight > rule.maxSurfaceHeight) {
        errorOut = "structure placement: rule minSurfaceHeight > maxSurfaceHeight";
        return false;
    }
    if (rule.density < 0.0f || rule.density > 1.0f) {
        errorOut = "structure placement: rule density must be in [0, 1]";
        return false;
    }
    if (rule.spacing < 1) {
        errorOut = "structure placement: rule spacing must be >= 1";
        return false;
    }
    return true;
}

// ---- minimal JSON stringification (RegistryJson.hpp only parses) ----
// Used to re-encode the embedded canonical asset object so the structure
// generator's own deserialize is the single parser for spec documents.

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string json_stringify(const sdk::JsonValue& value) {
    switch (value.kind) {
        case sdk::JsonValue::Kind::Null: return "null";
        case sdk::JsonValue::Kind::Bool:
            return value.boolean ? "true" : "false";
        case sdk::JsonValue::Kind::Number: {
            std::ostringstream ss;
            // 17 digits round-trips any double exactly; integers print
            // without a fractional part.
            ss.precision(17);
            ss << value.number;
            return ss.str();
        }
        case sdk::JsonValue::Kind::String:
            return "\"" + json_escape(value.string) + "\"";
        case sdk::JsonValue::Kind::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) out += ",";
                out += json_stringify(value.array[i]);
            }
            return out + "]";
        }
        case sdk::JsonValue::Kind::Object: {
            std::string out = "{";
            bool first = true;
            for (const auto& entry : value.object) {
                if (!first) out += ",";
                first = false;
                out += "\"" + json_escape(entry.first) + "\":" +
                       json_stringify(entry.second);
            }
            return out + "}";
        }
    }
    return "null";
}

class StructurePlacementSystem final : public IStructurePlacementSystem {
public:
    bool add_definition(const StructureDefinition& definition,
                        std::string& errorOut) override {
        errorOut.clear();
        if (!valid_definition(definition, errorOut)) return false;
        if (defs_.find(definition.id) != defs_.end()) {
            errorOut =
                "structure placement: duplicate definition id '" + definition.id + "'";
            return false;
        }
        defs_[definition.id] = definition;
        return true;
    }

    const StructureDefinition* definition(const std::string& id) const override {
        const auto found = defs_.find(id);
        return found == defs_.end() ? nullptr : &found->second;
    }

    std::vector<std::string> definition_ids() const override {
        std::vector<std::string> ids;
        ids.reserve(defs_.size());
        for (const auto& entry : defs_) ids.push_back(entry.first);
        return ids;
    }

    const std::vector<StructureSpawnRule>& rules() const override { return rules_; }

    bool set_rules(const std::vector<StructureSpawnRule>& rules,
                   std::string& errorOut) override {
        errorOut.clear();
        for (const auto& rule : rules) {
            if (!valid_rule(rule, defs_, errorOut)) return false;
        }
        rules_ = rules;
        return true;
    }

    bool try_place(const std::vector<StructureSpawnRule>& rules, int worldX,
                   int worldZ, int surfaceHeight, const std::string& biomeName,
                   std::uint32_t worldSeed, StructurePlacement& out,
                   std::string& errorOut) const override {
        errorOut.clear();
        const std::vector<StructureSpawnRule>* active = &rules;
        if (active->empty()) active = &rules_;
        for (const auto& rule : *active) {
            // Biome gate: biomeName "" = no biome info, so only rules without
            // a biome gate can match.
            if (!rule.biomes.empty()) {
                if (biomeName.empty()) continue;
                const bool inBiome =
                    std::find(rule.biomes.begin(), rule.biomes.end(), biomeName) !=
                    rule.biomes.end();
                if (!inBiome) continue;
            }
            if (surfaceHeight < rule.minSurfaceHeight ||
                surfaceHeight > rule.maxSurfaceHeight) {
                continue;
            }
            if (rule.spacing < 1) {
                errorOut = "structure placement: rule spacing must be >= 1";
                return false;
            }
            // The candidate cell is derived from the column by the rule's
            // spacing; the placement origin is the CELL ORIGIN, so querying at
            // the cell origin is canonical.
            const std::int64_t cellX = cell_origin(worldX, rule.spacing);
            const std::int64_t cellZ = cell_origin(worldZ, rule.spacing);
            if (rule.density <= 0.0f) continue;
            if (rule.density < 1.0f) {
                const std::uint32_t threshold = static_cast<std::uint32_t>(
                    rule.density * 10000.0f + 0.5f);
                if (cell_hash(worldSeed, rule.seedOffset, cellX, cellZ) % 10000 >=
                    threshold) {
                    continue;
                }
            }
            const auto found = defs_.find(rule.structureId);
            if (found == defs_.end()) {
                errorOut = "structure placement: rule references unknown "
                           "structureId '" +
                           rule.structureId + "'";
                return false;
            }
            std::string error;
            auto generator = create_structure_generator(found->second.spec, error);
            if (!generator) {
                errorOut = "structure placement: invalid structure asset for '" +
                           rule.structureId + "': " + error;
                return false;
            }
            const int outWidth = found->second.outputWidth > 0
                                     ? found->second.outputWidth
                                     : found->second.spec.sampleWidth;
            const int outHeight = found->second.outputHeight > 0
                                      ? found->second.outputHeight
                                      : found->second.spec.sampleHeight;
            StructureOutput output;
            if (!generator->generate(outWidth, outHeight, output, error)) {
                errorOut = "structure placement: generation failed for '" +
                           rule.structureId + "': " + error;
                return false;
            }
            out.structureId = rule.structureId;
            out.origin = glm::ivec3(static_cast<int>(cellX),
                                    surfaceHeight + rule.yOffset,
                                    static_cast<int>(cellZ));
            out.placementSeed = placement_seed(worldSeed, rule.seedOffset, cellX,
                                               cellZ);
            out.output = std::move(output);
            return true;
        }
        return false;  // no rule matched — normal outcome, no diagnostic
    }

    bool plan_region(const std::vector<StructureSpawnRule>& rules, int minChunkX,
                     int minChunkZ, int chunkCountX, int chunkCountZ,
                     const std::function<int(int, int)>& surfaceAt,
                     const std::function<std::string(int, int)>& biomeAt,
                     std::uint32_t worldSeed, std::vector<StructurePlacement>& out,
                     std::string& errorOut) const override {
        errorOut.clear();
        const std::vector<StructureSpawnRule>* active = &rules;
        if (active->empty()) active = &rules_;
        out.clear();
        if (active->empty()) return true;
        if (chunkCountX <= 0 || chunkCountZ <= 0) {
            errorOut = "structure placement: region chunk counts must be positive";
            return false;
        }
        // Dedupe key (structureId, origin): a placement derived from a rule
        // with a coarse spacing can be reachable from a query column in a
        // neighbouring chunk, so the same placement can be evaluated more than
        // once. (structureId, origin) is unique per placement.
        std::set<std::tuple<std::string, int, int, int>> seen;
        for (int cz = 0; cz < chunkCountZ; ++cz) {
            for (int cx = 0; cx < chunkCountX; ++cx) {
                const int chunkMinX = (minChunkX + cx) * 16;
                const int chunkMinZ = (minChunkZ + cz) * 16;
                // The candidate cell origins of EVERY rule's spacing that
                // intersect this chunk, sorted — a fixed iteration order makes
                // the plan deterministic.
                std::set<std::pair<std::int64_t, std::int64_t>> cells;
                for (const auto& rule : *active) {
                    if (rule.spacing < 1) {
                        errorOut = "structure placement: rule spacing must be >= 1";
                        return false;
                    }
                    for (std::int64_t gx = cell_origin(chunkMinX, rule.spacing);
                         gx < chunkMinX + 16; gx += rule.spacing) {
                        for (std::int64_t gz = cell_origin(chunkMinZ, rule.spacing);
                             gz < chunkMinZ + 16; gz += rule.spacing) {
                            cells.emplace(gx, gz);
                        }
                    }
                }
                for (const auto& cell : cells) {
                    StructurePlacement placement;
                    std::string error;
                    // surfaceAt/biomeAt are sampled at the candidate cell
                    // origin (the canonical query point).
                    if (try_place(*active, static_cast<int>(cell.first),
                                  static_cast<int>(cell.second),
                                  surfaceAt(static_cast<int>(cell.first),
                                            static_cast<int>(cell.second)),
                                  biomeAt(static_cast<int>(cell.first),
                                          static_cast<int>(cell.second)),
                                  worldSeed, placement, error)) {
                        const auto key = std::make_tuple(
                            placement.structureId, placement.origin.x,
                            placement.origin.y, placement.origin.z);
                        if (seen.insert(key).second) {
                            out.push_back(std::move(placement));
                        }
                    } else if (!error.empty()) {
                        // Hard error (unknown structureId, generation failure):
                        // abort the whole plan rather than skip silently.
                        errorOut = error;
                        return false;
                    }
                }
            }
        }
        return true;
    }

    bool resolve_sockets(const StructurePlacement& placement,
                         std::vector<StructureSocket>& out,
                         std::string& errorOut) const override {
        errorOut.clear();
        const StructureDefinition* def = definition(placement.structureId);
        if (def == nullptr) {
            errorOut = "structure placement: placement references unknown "
                       "structure '" +
                       placement.structureId + "'";
            return false;
        }
        out.clear();
        out.reserve(def->sockets.size());
        for (const auto& socket : def->sockets) {
            StructureSocket world = socket;
            world.position += placement.origin;
            out.push_back(world);
        }
        return true;
    }

    bool connect_sockets(const StructurePlacement& a, const std::string& socketA,
                         const StructurePlacement& b, const std::string& socketB,
                         glm::ivec3& bOriginOut, std::string& errorOut) const override {
        errorOut.clear();
        const StructureDefinition* defA = definition(a.structureId);
        if (defA == nullptr) {
            errorOut = "structure placement: placement a references unknown "
                       "structure '" +
                       a.structureId + "'";
            return false;
        }
        const StructureDefinition* defB = definition(b.structureId);
        if (defB == nullptr) {
            errorOut = "structure placement: placement b references unknown "
                       "structure '" +
                       b.structureId + "'";
            return false;
        }
        const StructureSocket* sa = find_socket(*defA, socketA);
        if (sa == nullptr) {
            errorOut = "structure placement: structure '" + a.structureId +
                       "' has no socket '" + socketA + "'";
            return false;
        }
        const StructureSocket* sb = find_socket(*defB, socketB);
        if (sb == nullptr) {
            errorOut = "structure placement: structure '" + b.structureId +
                       "' has no socket '" + socketB + "'";
            return false;
        }
        if (!sa->connectTag.empty() && !sb->connectTag.empty() &&
            sa->connectTag != sb->connectTag) {
            errorOut = "structure placement: socket tags do not match ('" +
                       sa->connectTag + "' vs '" + sb->connectTag + "')";
            return false;
        }
        if (!facings_oppose(sa->facing, sb->facing)) {
            errorOut = "structure placement: socket facings do not oppose";
            return false;
        }
        // b's socket position must coincide with a's socket position in world
        // space: a.origin + sa->position == bOrigin + sb->position.
        bOriginOut = a.origin + sa->position - sb->position;
        return true;
    }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss.precision(9);  // exact float round-trip for density
        ss << "{\"version\":1,\"definitions\":[";
        bool firstDef = true;
        for (const auto& entry : defs_) {
            if (!firstDef) ss << ',';
            firstDef = false;
            const StructureDefinition& def = entry.second;
            std::string assetJson;
            std::string error;
            auto generator = create_structure_generator(def.spec, error);
            if (!generator || !generator->serialize(assetJson)) {
                out.clear();
                return false;
            }
            ss << "{\"id\":\"" << json_escape(def.id) << "\""
               << ",\"outputWidth\":" << def.outputWidth
               << ",\"outputHeight\":" << def.outputHeight
               << ",\"spec\":" << assetJson << ",\"sockets\":[";
            for (std::size_t i = 0; i < def.sockets.size(); ++i) {
                if (i != 0) ss << ',';
                const StructureSocket& socket = def.sockets[i];
                ss << "{\"name\":\"" << json_escape(socket.name) << "\""
                   << ",\"position\":[" << socket.position.x << ','
                   << socket.position.y << ',' << socket.position.z << "]"
                   << ",\"facing\":" << socket.facing
                   << ",\"connectTag\":\"" << json_escape(socket.connectTag)
                   << "\"}";
            }
            ss << "]}";
        }
        ss << "],\"rules\":[";
        for (std::size_t i = 0; i < rules_.size(); ++i) {
            if (i != 0) ss << ',';
            const StructureSpawnRule& rule = rules_[i];
            ss << "{\"structureId\":\"" << json_escape(rule.structureId)
               << "\",\"biomes\":[";
            for (std::size_t b = 0; b < rule.biomes.size(); ++b) {
                if (b != 0) ss << ',';
                ss << "\"" << json_escape(rule.biomes[b]) << "\"";
            }
            ss << "],\"minSurfaceHeight\":" << rule.minSurfaceHeight
               << ",\"maxSurfaceHeight\":" << rule.maxSurfaceHeight
               << ",\"density\":" << rule.density
               << ",\"spacing\":" << rule.spacing
               << ",\"yOffset\":" << rule.yOffset
               << ",\"seedOffset\":" << rule.seedOffset << "}";
        }
        ss << "]}";
        out = ss.str();
        return true;
    }

    bool deserialize(const std::string& json, std::string& errorOut) override {
        errorOut.clear();
        sdk::JsonValue document;
        if (!sdk::json_parse(json, document, errorOut)) {
            errorOut = "structure placement: malformed document - " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "structure placement: document must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "structure placement: unsupported document version";
            return false;
        }
        // Parse into temp state; commit only at the end (all-or-nothing).
        std::map<std::string, StructureDefinition> parsedDefs;
        std::vector<StructureSpawnRule> parsedRules;
        if (const sdk::JsonValue* defs = document.field("definitions")) {
            if (!defs->is_array()) {
                errorOut = "structure placement: \"definitions\" must be an array";
                return false;
            }
            for (const sdk::JsonValue& entry : defs->array) {
                if (!entry.is_object()) {
                    errorOut = "structure placement: definition entry must be an "
                               "object";
                    return false;
                }
                StructureDefinition def;
                def.id = sdk::json_string(entry, "id", "");
                def.outputWidth =
                    static_cast<int>(sdk::json_number(entry, "outputWidth", 0.0));
                def.outputHeight =
                    static_cast<int>(sdk::json_number(entry, "outputHeight", 0.0));
                const sdk::JsonValue* spec = entry.field("spec");
                if (spec == nullptr || !spec->is_object()) {
                    errorOut = "structure placement: definition '" + def.id +
                               "' has no \"spec\" object";
                    return false;
                }
                // Re-encode the canonical asset object and let the structure
                // generator's own deserialize be the single spec parser.
                std::string error;
                auto generator = create_structure_generator_from_json(
                    json_stringify(*spec), error);
                if (!generator) {
                    errorOut = "structure placement: invalid structure asset for '" +
                               def.id + "': " + error;
                    return false;
                }
                def.spec = generator->asset();
                if (const sdk::JsonValue* sockets = entry.field("sockets")) {
                    if (!sockets->is_array()) {
                        errorOut = "structure placement: \"sockets\" must be an "
                                   "array";
                        return false;
                    }
                    for (const sdk::JsonValue& socketEntry : sockets->array) {
                        if (!socketEntry.is_object()) {
                            errorOut = "structure placement: socket entry must be "
                                       "an object";
                            return false;
                        }
                        StructureSocket socket;
                        socket.name = sdk::json_string(socketEntry, "name", "");
                        const std::vector<double> position =
                            sdk::json_number_array(socketEntry, "position");
                        if (position.size() == 3) {
                            socket.position = glm::ivec3(
                                static_cast<int>(position[0]),
                                static_cast<int>(position[1]),
                                static_cast<int>(position[2]));
                        }
                        socket.facing = static_cast<int>(
                            sdk::json_number(socketEntry, "facing", 0.0));
                        socket.connectTag =
                            sdk::json_string(socketEntry, "connectTag", "");
                        def.sockets.push_back(socket);
                    }
                }
                if (!valid_definition(def, errorOut)) return false;
                if (parsedDefs.find(def.id) != parsedDefs.end()) {
                    errorOut = "structure placement: duplicate definition id '" +
                               def.id + "'";
                    return false;
                }
                parsedDefs[def.id] = std::move(def);
            }
        }
        if (const sdk::JsonValue* rules = document.field("rules")) {
            if (!rules->is_array()) {
                errorOut = "structure placement: \"rules\" must be an array";
                return false;
            }
            for (const sdk::JsonValue& entry : rules->array) {
                if (!entry.is_object()) {
                    errorOut = "structure placement: rule entry must be an object";
                    return false;
                }
                StructureSpawnRule rule;
                rule.structureId = sdk::json_string(entry, "structureId", "");
                rule.biomes = sdk::json_string_array(entry, "biomes");
                rule.minSurfaceHeight = static_cast<int>(
                    sdk::json_number(entry, "minSurfaceHeight",
                                     static_cast<double>(rule.minSurfaceHeight)));
                rule.maxSurfaceHeight = static_cast<int>(
                    sdk::json_number(entry, "maxSurfaceHeight",
                                     static_cast<double>(rule.maxSurfaceHeight)));
                rule.density = static_cast<float>(
                    sdk::json_number(entry, "density", 0.0));
                rule.spacing = static_cast<int>(
                    sdk::json_number(entry, "spacing", 8.0));
                rule.yOffset = static_cast<int>(
                    sdk::json_number(entry, "yOffset", 1.0));
                rule.seedOffset = static_cast<std::uint32_t>(
                    sdk::json_number(entry, "seedOffset", 0.0));
                parsedRules.push_back(rule);
            }
        }
        // Dangling rule references are refused against the PARSED definitions
        // (all-or-nothing: a bad rule rejects the whole document).
        for (const auto& rule : parsedRules) {
            if (!valid_rule(rule, parsedDefs, errorOut)) return false;
        }
        // Commit.
        defs_ = std::move(parsedDefs);
        rules_ = std::move(parsedRules);
        return true;
    }

private:
    std::map<std::string, StructureDefinition> defs_;
    std::vector<StructureSpawnRule> rules_;
};

}  // namespace

std::shared_ptr<IStructurePlacementSystem> create_structure_placement_system() {
    return std::make_shared<StructurePlacementSystem>();
}

std::shared_ptr<IStructurePlacementSystem> create_structure_placement_system_from_json(
    const std::string& json, std::string& errorOut) {
    errorOut.clear();
    auto system = std::make_shared<StructurePlacementSystem>();
    if (!system->deserialize(json, errorOut)) return nullptr;
    return system;
}

}  // namespace procgen
}  // namespace engine
