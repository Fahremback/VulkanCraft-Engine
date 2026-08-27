// CrowdSimulation.cpp — the only TU implementing the public crowd contract
// (Agente 4 §10 l.176 "Integrar multidões com LOD de simulação, sleeping,
// agregação distante e retomada determinística"). Deterministic, pure std.
// RegistryJson only for the parser.

#include "engine/ai/ICrowdSimulation.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace engine {
namespace ai {
namespace {

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

bool is_number(const sdk::JsonValue* v) {
    return v != nullptr && v->kind == sdk::JsonValue::Kind::Number;
}

bool is_string(const sdk::JsonValue* v) {
    return v != nullptr && v->kind == sdk::JsonValue::Kind::String;
}

std::string fmt_double(double value) {
    // bit-exact %.9g
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", value);
    return buf;
}

CrowdTier tier_for(const CrowdSpec& spec, double distance) {
    if (distance <= spec.full_radius) return CrowdTier::Full;
    if (distance <= spec.reduced_radius) return CrowdTier::Reduced;
    if (distance <= spec.aggregate_radius) return CrowdTier::Aggregate;
    return CrowdTier::Dormant;
}

double distance_to(const Vec3& a, const Vec3& b) {
    const double dx = static_cast<double>(a.x) - static_cast<double>(b.x);
    const double dy = static_cast<double>(a.y) - static_cast<double>(b.y);
    const double dz = static_cast<double>(a.z) - static_cast<double>(b.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

class CrowdSimulation final : public ICrowdSimulation {
public:
    CrowdSimulation() = default;

    bool configure(const CrowdSpec& spec, std::string& errorOut) override {
        CrowdSpec parsed = spec;
        if (!parsed.validate(errorOut)) return false;
        spec_ = parsed;
        return true;
    }

    bool set_agents(const std::vector<CrowdAgent>& agents,
                    std::string& errorOut) override {
        if (agents.size() > spec_.max_agents) {
            errorOut = "crowd: population exceeds max_agents";
            return false;
        }
        std::vector<CrowdAgent> sorted = agents;
        std::sort(sorted.begin(), sorted.end(),
                  [](const CrowdAgent& a, const CrowdAgent& b) {
                      return a.id < b.id;
                  });
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            if (sorted[i].id == sorted[i - 1].id) {
                errorOut = "crowd: duplicate agent id";
                return false;
            }
        }
        agents_.clear();
        states_.clear();
        for (const CrowdAgent& a : sorted) {
            agents_.push_back(a);
            CrowdAgentState st;
            st.id = a.id;
            st.tier = CrowdTier::Dormant;
            states_[a.id] = st;
        }
        tick_ = 0;
        return true;
    }

    void remove_agent(std::uint64_t id) override {
        auto it = states_.find(id);
        if (it == states_.end()) return;
        states_.erase(it);
        agents_.erase(
            std::remove_if(agents_.begin(), agents_.end(),
                           [id](const CrowdAgent& a) { return a.id == id; }),
            agents_.end());
    }

    std::size_t agent_count() const override { return agents_.size(); }

    CrowdFrameResult advance(const Vec3& focus, std::uint32_t frames,
                             std::string& errorOut) override {
        CrowdFrameResult last;
        if (frames == 0) {
            errorOut = "crowd: frames must be > 0";
            return last;
        }
        if (!spec_.validate(errorOut)) return last;
        bool woke = false;
        for (std::uint32_t f = 0; f < frames; ++f) {
            last = advance_one(focus, woke);
            ++tick_;
        }
        last.woke_any = woke;
        return last;
    }

    bool agent_state(std::uint64_t id, CrowdAgentState& out) const override {
        auto it = states_.find(id);
        if (it == states_.end()) return false;
        out = it->second;
        return true;
    }

    std::string serialize_state() const override {
        std::ostringstream os;
        os << "{\"version\":1,\"tick\":" << tick_ << ",\"agents\":[";
        bool first = true;
        for (const CrowdAgent& a : agents_) {
            if (!first) os << ',';
            first = false;
            const auto it = states_.find(a.id);
            const CrowdAgentState& st = it->second;
            os << "{\"id\":" << a.id << ",\"type\":\"" << json_escape(a.type)
               << "\",\"tier\":" << static_cast<int>(st.tier)
               << ",\"idle\":" << st.idle_ticks << ",\"x\":" << fmt_double(a.position.x)
               << ",\"y\":" << fmt_double(a.position.y)
               << ",\"z\":" << fmt_double(a.position.z) << "}";
        }
        os << "],\"aggregates\":[";
        first = true;
        for (const auto& agg : aggregates_) {
            if (!first) os << ',';
            first = false;
            os << "{\"type\":\"" << json_escape(agg.first) << "\",\"count\":"
               << agg.second.count << ",\"activity\":" << fmt_double(agg.second.activity)
               << "}";
        }
        os << "]}";
        return os.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue root;
        std::string perr;
        if (!sdk::json_parse(json, root, perr) || !root.is_object()) {
            errorOut = "crowd: " + perr;
            return false;
        }
        const sdk::JsonValue* version = root.field("version");
        if (!is_number(version) || static_cast<long long>(version->number) != 1) {
            errorOut = "crowd: version must be 1";
            return false;
        }
        const sdk::JsonValue* tick = root.field("tick");
        if (!is_number(tick) || tick->number < 0) {
            errorOut = "crowd: invalid tick";
            return false;
        }
        const sdk::JsonValue* agents = root.field("agents");
        if (!agents || agents->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "crowd: agents must be an array";
            return false;
        }
        std::vector<CrowdAgent> parsedAgents;
        std::map<std::uint64_t, CrowdAgentState> parsedStates;
        for (const sdk::JsonValue& el : agents->array) {
            if (!el.is_object()) {
                errorOut = "crowd: agent entry must be an object";
                return false;
            }
            const sdk::JsonValue* idv = el.field("id");
            const sdk::JsonValue* typev = el.field("type");
            const sdk::JsonValue* tierv = el.field("tier");
            const sdk::JsonValue* idlev = el.field("idle");
            const sdk::JsonValue* xv = el.field("x");
            const sdk::JsonValue* yv = el.field("y");
            const sdk::JsonValue* zv = el.field("z");
            if (!is_number(idv) || !is_string(typev) || !is_number(tierv) ||
                !is_number(idlev) || !is_number(xv) || !is_number(yv) ||
                !is_number(zv)) {
                errorOut = "crowd: malformed agent entry";
                return false;
            }
            const std::uint64_t id = static_cast<std::uint64_t>(idv->number);
            const int tierInt = static_cast<int>(tierv->number);
            if (tierInt < 0 || tierInt > 3) {
                errorOut = "crowd: invalid tier";
                return false;
            }
            if (idlev->number < 0) {
                errorOut = "crowd: invalid idle ticks";
                return false;
            }
            if (parsedStates.count(id) != 0) {
                errorOut = "crowd: duplicate agent id in state";
                return false;
            }
            CrowdAgent a;
            a.id = id;
            a.type = typev->string;
            a.position.x = xv->number;
            a.position.y = yv->number;
            a.position.z = zv->number;
            CrowdAgentState st;
            st.id = id;
            st.tier = static_cast<CrowdTier>(tierInt);
            st.idle_ticks = static_cast<std::uint32_t>(idlev->number);
            parsedAgents.push_back(a);
            parsedStates[id] = st;
        }
        // aggregates (opcional — contadores de grupos Aggregate)
        std::map<std::string, CrowdAggregate> parsedAggs;
        const sdk::JsonValue* aggs = root.field("aggregates");
        if (aggs) {
            if (aggs->kind != sdk::JsonValue::Kind::Array) {
                errorOut = "crowd: aggregates must be an array";
                return false;
            }
            for (const sdk::JsonValue& el : aggs->array) {
                if (!el.is_object()) {
                    errorOut = "crowd: aggregate entry must be an object";
                    return false;
                }
                const sdk::JsonValue* tv = el.field("type");
                const sdk::JsonValue* cv = el.field("count");
                const sdk::JsonValue* av = el.field("activity");
                if (!is_string(tv) || !is_number(cv) || !is_number(av) ||
                    cv->number < 0) {
                    errorOut = "crowd: malformed aggregate entry";
                    return false;
                }
                CrowdAggregate agg;
                agg.type = tv->string;
                agg.count = static_cast<std::uint64_t>(cv->number);
                agg.activity = av->number;
                parsedAggs[agg.type] = agg;
            }
        }
        // commit all-or-nothing
        agents_ = parsedAgents;
        states_ = parsedStates;
        aggregates_ = parsedAggs;
        tick_ = static_cast<std::uint64_t>(tick->number);
        return true;
    }

private:
    CrowdFrameResult advance_one(const Vec3& focus, bool& wokeAny) {
        CrowdFrameResult result;
        const std::uint64_t frame = tick_;

        // 1) Re-classifica tiers + seleciona quem ticka (budget respeitado).
        std::vector<std::uint64_t> tickIds;
        for (const CrowdAgent& a : agents_) {
            const double dist = distance_to(a.position, focus);
            CrowdTier tier = tier_for(spec_, dist);
            CrowdAgentState st = states_[a.id];
            st.id = a.id;
            const bool wasDormant = st.tier == CrowdTier::Dormant;
            st.tier = tier;
            if (tier == CrowdTier::Full) {
                st.idle_ticks = 0;
                tickIds.push_back(a.id);
            } else if (tier == CrowdTier::Reduced) {
                st.idle_ticks = 0;
                if (frame % static_cast<std::uint64_t>(spec_.reduced_interval) == 0) {
                    tickIds.push_back(a.id);
                }
            } else if (tier == CrowdTier::Dormant) {
                st.idle_ticks += 1;
                // transição para Dormant (acordando) é registrada abaixo;
                // aqui apenas acumula dormência se já estava dormindo
            }
            // Acordar: estava Dormant e agora é Full/Reduced/Aggregate → o
            // foco se aproximou (ou o agente voltou à relevância).
            if (wasDormant && tier != CrowdTier::Dormant) {
                wokeAny = true;
                st.idle_ticks = 0;
            }
            // Aggregate: não ticka (evoluído pelo modelo analítico abaixo)
            states_[a.id] = st;
        }

        // 2) Budget de ticks: excesso → os MAIS DISTANTES ficam para o
        //    próximo frame; ficam os MAIS PRÓXIMOS (ordenação por distância
        //    asc, id asc — determinístico).
        std::vector<std::uint64_t> keepIds = tickIds;
        if (spec_.max_ticks_per_frame > 0 &&
            keepIds.size() > spec_.max_ticks_per_frame) {
            std::vector<std::pair<double, std::uint64_t>> distOrder;
            for (const std::uint64_t id : tickIds) {
                const CrowdAgent* ag = find_agent(id);
                const double d = distance_to(ag->position, focus);
                distOrder.push_back({ d, id });
            }
            std::sort(distOrder.begin(), distOrder.end(),
                      [](const std::pair<double, std::uint64_t>& a,
                         const std::pair<double, std::uint64_t>& b) {
                          if (a.first != b.first) return a.first < b.first;
                          return a.second < b.second;
                      });
            keepIds.clear();
            for (std::size_t i = 0; i < spec_.max_ticks_per_frame; ++i) {
                keepIds.push_back(distOrder[i].second);
            }
        }

        // 3) Monta o resultado por agente (ordenado por id).
        for (const CrowdAgent& a : agents_) {
            CrowdAgentState st = states_[a.id];
            st.tick_this_frame =
                std::find(keepIds.begin(), keepIds.end(), a.id) != keepIds.end();
            result.agent_states.push_back(st);
        }

        // 4) Agregação: grupos Aggregate por tipo → contadores analíticos.
        aggregates_.clear();
        std::map<std::string, std::pair<std::uint64_t, double>> byType;
        for (const CrowdAgent& a : agents_) {
            const CrowdTier t = states_[a.id].tier;
            if (t != CrowdTier::Aggregate) continue;
            auto& acc = byType[a.type];
            ++acc.first;
            // modelo analítico determinístico (sem RNG — splitmix64 do id + tick)
            acc.second += analytic_growth(a.id, tick_);
        }
        for (const auto& kv : byType) {
            CrowdAggregate agg;
            agg.type = kv.first;
            agg.count = kv.second.first;
            agg.activity = kv.second.second;
            aggregates_[kv.first] = agg;
            result.aggregates.push_back(agg);
        }
        std::sort(result.aggregates.begin(), result.aggregates.end(),
                  [](const CrowdAggregate& a, const CrowdAggregate& b) {
                      return a.type < b.type;
                  });
        return result;
    }

    const CrowdAgent* find_agent(std::uint64_t id) const {
        for (const CrowdAgent& a : agents_) {
            if (a.id == id) return &a;
        }
        return nullptr;
    }

    static double analytic_growth(std::uint64_t id, std::uint64_t tick) {
        // splitmix64 determinístico — mesmo padrão do ISimulationLod
        std::uint64_t z = id + 0x9E3779B97F4A7C15ULL + (tick << 1);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        return static_cast<double>((z >> 32) & 0xFFFF) / 65535.0;
    }

    CrowdSpec spec_;
    std::vector<CrowdAgent> agents_;          // ordenado por id
    std::map<std::uint64_t, CrowdAgentState> states_;
    std::map<std::string, CrowdAggregate> aggregates_;
    std::uint64_t tick_ = 0;
};

}  // namespace

bool CrowdSpec::validate(std::string& errorOut) const {
    if (!(full_radius >= 0.0)) {
        errorOut = "crowd: full_radius must be >= 0";
        return false;
    }
    if (reduced_radius < full_radius) {
        errorOut = "crowd: reduced_radius must be >= full_radius";
        return false;
    }
    if (aggregate_radius < reduced_radius) {
        errorOut = "crowd: aggregate_radius must be >= reduced_radius";
        return false;
    }
    if (reduced_interval < 1.0) {
        errorOut = "crowd: reduced_interval must be >= 1";
        return false;
    }
    if (max_agents == 0) {
        errorOut = "crowd: max_agents must be >= 1";
        return false;
    }
    return true;
}

bool CrowdSpec::load_from_json(const std::string& json, std::string& errorOut) {
    sdk::JsonValue root;
    std::string perr;
    if (!sdk::json_parse(json, root, perr) || !root.is_object()) {
        errorOut = "crowd: " + perr;
        return false;
    }
    CrowdSpec parsed = *this;
    if (const sdk::JsonValue* v = root.field("full_radius")) {
        if (!is_number(v)) { errorOut = "crowd: full_radius must be a number"; return false; }
        parsed.full_radius = v->number;
    }
    if (const sdk::JsonValue* v = root.field("reduced_radius")) {
        if (!is_number(v)) { errorOut = "crowd: reduced_radius must be a number"; return false; }
        parsed.reduced_radius = v->number;
    }
    if (const sdk::JsonValue* v = root.field("aggregate_radius")) {
        if (!is_number(v)) { errorOut = "crowd: aggregate_radius must be a number"; return false; }
        parsed.aggregate_radius = v->number;
    }
    if (const sdk::JsonValue* v = root.field("reduced_interval")) {
        if (!is_number(v)) { errorOut = "crowd: reduced_interval must be a number"; return false; }
        parsed.reduced_interval = v->number;
    }
    if (const sdk::JsonValue* v = root.field("max_agents")) {
        if (!is_number(v)) { errorOut = "crowd: max_agents must be a number"; return false; }
        parsed.max_agents = static_cast<std::uint32_t>(v->number);
    }
    if (const sdk::JsonValue* v = root.field("max_ticks_per_frame")) {
        if (!is_number(v)) { errorOut = "crowd: max_ticks_per_frame must be a number"; return false; }
        parsed.max_ticks_per_frame = static_cast<std::uint32_t>(v->number);
    }
    if (!parsed.validate(errorOut)) return false;
    *this = parsed;
    return true;
}

std::string CrowdSpec::to_json() const {
    std::ostringstream os;
    os << "{\"full_radius\":" << fmt_double(full_radius)
       << ",\"reduced_radius\":" << fmt_double(reduced_radius)
       << ",\"aggregate_radius\":" << fmt_double(aggregate_radius)
       << ",\"reduced_interval\":" << fmt_double(reduced_interval)
       << ",\"max_agents\":" << max_agents
       << ",\"max_ticks_per_frame\":" << max_ticks_per_frame << "}";
    return os.str();
}

std::unique_ptr<ICrowdSimulation> create_crowd_simulation() {
    return std::make_unique<CrowdSimulation>();
}

}  // namespace ai
}  // namespace engine
