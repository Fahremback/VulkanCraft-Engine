#include "engine/ai/IAiLod.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace engine {
namespace ai {
namespace {

bool finite(double v) {
    return std::isfinite(v);
}

bool is_uint64(const sdk::JsonValue& v) {
    return v.kind == sdk::JsonValue::Kind::Number && v.number >= 0.0 &&
           v.number == std::floor(v.number);
}

bool number_field(const sdk::JsonValue& obj, const char* key, double& out,
                  bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Number) {
        errorOut = std::string(key) + " must be a number";
        return false;
    }
    out = f->number;
    return true;
}

bool int_field(const sdk::JsonValue& obj, const char* key, int& out,
               std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        return true;
    }
    if (!is_uint64(*f)) {
        errorOut = std::string(key) + " must be a non-negative integer";
        return false;
    }
    out = static_cast<int>(f->number);
    return true;
}

}  // namespace

bool AiLodSpec::validate(std::string& errorOut) const {
    if (!finite(full_radius) || full_radius < 0.0) {
        errorOut = "full_radius must be finite and >= 0";
        return false;
    }
    if (!finite(reduced_radius) || reduced_radius < full_radius) {
        errorOut = "reduced_radius must be finite and >= full_radius";
        return false;
    }
    if (!finite(aggregate_radius) || aggregate_radius < reduced_radius) {
        errorOut = "aggregate_radius must be finite and >= reduced_radius";
        return false;
    }
    if (!finite(reduced_interval) || reduced_interval < 1.0) {
        errorOut = "reduced_interval must be finite and >= 1";
        return false;
    }
    if (!finite(aggregate_interval) || aggregate_interval < 1.0) {
        errorOut = "aggregate_interval must be finite and >= 1";
        return false;
    }
    if (max_full < 0 || max_reduced < 0 || max_aggregate < 0) {
        errorOut = "budgets must be >= 0";
        return false;
    }
    errorOut.clear();
    return true;
}

bool AiLodSpec::load_from_json(const std::string& json, std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "ai lod spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported ai lod spec version";
        return false;
    }
    AiLodSpec candidate;
    if (!number_field(doc, "full_radius", candidate.full_radius, false, errorOut)) return false;
    if (!number_field(doc, "reduced_radius", candidate.reduced_radius, false, errorOut)) return false;
    if (!number_field(doc, "aggregate_radius", candidate.aggregate_radius, false, errorOut)) return false;
    if (!number_field(doc, "reduced_interval", candidate.reduced_interval, false, errorOut)) return false;
    if (!number_field(doc, "aggregate_interval", candidate.aggregate_interval, false, errorOut)) return false;
    if (!int_field(doc, "max_full", candidate.max_full, errorOut)) return false;
    if (!int_field(doc, "max_reduced", candidate.max_reduced, errorOut)) return false;
    if (!int_field(doc, "max_aggregate", candidate.max_aggregate, errorOut)) return false;
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = candidate;
    return true;
}

std::string AiLodSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1"
        << ",\"full_radius\":" << full_radius
        << ",\"reduced_radius\":" << reduced_radius
        << ",\"aggregate_radius\":" << aggregate_radius
        << ",\"reduced_interval\":" << reduced_interval
        << ",\"aggregate_interval\":" << aggregate_interval
        << ",\"max_full\":" << max_full
        << ",\"max_reduced\":" << max_reduced
        << ",\"max_aggregate\":" << max_aggregate << "}";
    return out.str();
}

namespace {

class AiLod final : public IAiLod {
public:
    bool configure(const AiLodSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        return true;
    }

    AiLodTier tier_for(double distance) const override {
        if (distance <= spec_.full_radius) {
            return AiLodTier::Full;
        }
        if (distance <= spec_.reduced_radius) {
            return AiLodTier::Reduced;
        }
        if (distance <= spec_.aggregate_radius) {
            return AiLodTier::Aggregate;
        }
        return AiLodTier::Dormant;
    }

    bool should_update(AiLodTier tier, std::uint64_t tick_index) const override {
        switch (tier) {
            case AiLodTier::Full:
                return true;
            case AiLodTier::Reduced:
                return tick_index % static_cast<std::uint64_t>(spec_.reduced_interval) == 0;
            case AiLodTier::Aggregate:
                return tick_index % static_cast<std::uint64_t>(spec_.aggregate_interval) == 0;
            case AiLodTier::Dormant:
                return false;
        }
        return false;
    }

    std::vector<AiLodAllocation> allocate(
        std::uint64_t tick_index,
        const std::vector<AiLodEntry>& entries) const override {
        // Classifica por tier; dentro de cada tier, ordena por (distância desc,
        // id desc) para o rebaixamento determinístico do excesso.
        std::vector<AiLodAllocation> out;
        out.reserve(entries.size());

        std::vector<AiLodEntry> full, reduced, aggregate, dormant;
        for (const auto& e : entries) {
            switch (tier_for(e.distance)) {
                case AiLodTier::Full: full.push_back(e); break;
                case AiLodTier::Reduced: reduced.push_back(e); break;
                case AiLodTier::Aggregate: aggregate.push_back(e); break;
                case AiLodTier::Dormant: dormant.push_back(e); break;
            }
        }

        // Ordenação de MANUTENÇÃO: mais próximo primeiro (o tier alto guarda
        // os mais próximos); o excesso (final da lista) é rebaixado. Empate →
        // menor id mantido primeiro (maior id rebaixado) — determinístico.
        const auto keep_order = [](const AiLodEntry& a, const AiLodEntry& b) {
            if (a.distance != b.distance) {
                return a.distance < b.distance;
            }
            return a.id < b.id;
        };
        std::sort(full.begin(), full.end(), keep_order);
        std::sort(reduced.begin(), reduced.end(), keep_order);
        std::sort(aggregate.begin(), aggregate.end(), keep_order);

        // Excesso do Full vai para Reduced (se houver budget), senão Aggregate...
        const int full_budget = spec_.max_full > 0 ? spec_.max_full : INT_MAX;
        const int reduced_budget = spec_.max_reduced > 0 ? spec_.max_reduced : INT_MAX;
        const int aggregate_budget = spec_.max_aggregate > 0 ? spec_.max_aggregate : INT_MAX;

        const auto demote_excess = [&](std::vector<AiLodEntry>& src,
                                       std::vector<AiLodEntry>& dst,
                                       int budget) {
            int kept = 0;
            for (auto it = src.begin(); it != src.end();) {
                if (kept < budget) {
                    ++kept;  // mais próximos ficam no tier alto
                    ++it;
                } else {
                    dst.push_back(*it);  // excesso (mais distantes) rebaixado
                    it = src.erase(it);  // e removido do tier de origem
                }
            }
        };

        demote_excess(full, reduced, full_budget);
        // Re-sort do pool do tier seguinte (recebeu o excesso do anterior)
        // antes de rebaixar o próprio excesso — mantém os mais próximos.
        std::sort(reduced.begin(), reduced.end(), keep_order);
        demote_excess(reduced, aggregate, reduced_budget);
        std::sort(aggregate.begin(), aggregate.end(), keep_order);
        demote_excess(aggregate, dormant, aggregate_budget);

        const auto emit = [&](const std::vector<AiLodEntry>& vec, AiLodTier tier) {
            for (const auto& e : vec) {
                AiLodAllocation a;
                a.id = e.id;
                a.tier = tier;
                a.update = should_update(tier, tick_index);
                out.push_back(a);
            }
        };
        emit(full, AiLodTier::Full);
        emit(reduced, AiLodTier::Reduced);
        emit(aggregate, AiLodTier::Aggregate);
        emit(dormant, AiLodTier::Dormant);
        return out;
    }

private:
    AiLodSpec spec_;
};

}  // namespace

std::unique_ptr<IAiLod> create_ai_lod() {
    return std::make_unique<AiLod>();
}

}  // namespace ai
}  // namespace engine
