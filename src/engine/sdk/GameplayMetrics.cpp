// GameplayMetrics.cpp — the only TU implementing the public gameplay metrics
// contract (Agente 4 §2 item 30 CORE): a registry of named metrics
// (Counter/Gauge/Sample) with deterministic snapshot and JSON output.
// Pure std; values use bit-level finite checks (/fp:fast guard, findings #79).

#include "engine/gameplay/IGameplayMetrics.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>

namespace engine {
namespace gameplay {
namespace {

bool finite_double(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double is 64-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    // IEEE-754: exponent all-ones => NaN or infinity.
    return (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

const char* kind_name(GameplayMetricKind kind) {
    switch (kind) {
        case GameplayMetricKind::Counter: return "counter";
        case GameplayMetricKind::Gauge: return "gauge";
        case GameplayMetricKind::Sample: return "sample";
    }
    return "counter";
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

std::string emit_double(double value) {
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
}

struct Metric {
    GameplayMetricKind kind{ GameplayMetricKind::Counter };
    double value{ 0.0 };
    std::uint64_t count{ 0 };
    double minValue{ 0.0 };
    double maxValue{ 0.0 };
};

class GameplayMetrics final : public IGameplayMetrics {
public:
    GameplayMetrics() = default;

    bool register_metric(const std::string& name, GameplayMetricKind kind,
                         std::string& errorOut) override {
        if (name.empty()) {
            errorOut = "gameplay metrics: name must be non-empty";
            return false;
        }
        if (metrics_.count(name) != 0) {
            errorOut = "gameplay metrics: duplicate metric '" + name + "'";
            return false;
        }
        Metric metric;
        metric.kind = kind;
        metrics_[name] = metric;
        return true;
    }

    bool record(const std::string& name, double value,
                std::string& errorOut) override {
        const auto found = metrics_.find(name);
        if (found == metrics_.end()) {
            errorOut = "gameplay metrics: unknown metric '" + name + "'";
            return false;
        }
        if (!finite_double(value)) {
            errorOut = "gameplay metrics: non-finite value for '" + name + "'";
            return false;
        }
        Metric& metric = found->second;
        switch (metric.kind) {
            case GameplayMetricKind::Counter:
                metric.value += value;
                break;
            case GameplayMetricKind::Gauge:
                metric.value = value;
                break;
            case GameplayMetricKind::Sample:
                if (metric.count == 0) {
                    metric.minValue = value;
                    metric.maxValue = value;
                } else {
                    metric.minValue = value < metric.minValue ? value : metric.minValue;
                    metric.maxValue = value > metric.maxValue ? value : metric.maxValue;
                }
                metric.value += value;  // soma; a média é value/count no snapshot
                break;
        }
        ++metric.count;
        return true;
    }

    std::vector<GameplayMetric> snapshot() const override {
        std::vector<GameplayMetric> out;
        out.reserve(metrics_.size());
        for (const auto& entry : metrics_) {  // std::map: ordem crescente de nome
            GameplayMetric metric;
            metric.name = entry.first;
            metric.kind = entry.second.kind;
            metric.count = entry.second.count;
            if (entry.second.kind == GameplayMetricKind::Sample &&
                entry.second.count > 0) {
                metric.value = entry.second.value /
                               static_cast<double>(entry.second.count);
            } else {
                metric.value = entry.second.value;
            }
            metric.minValue = entry.second.minValue;
            metric.maxValue = entry.second.maxValue;
            out.push_back(std::move(metric));
        }
        return out;
    }

    bool reset(const std::string& name) override {
        const auto found = metrics_.find(name);
        if (found == metrics_.end()) return false;
        const GameplayMetricKind kind = found->second.kind;  // preserva antes
        found->second = Metric{};
        found->second.kind = kind;
        return true;
    }

    void reset_all() override {
        for (auto& entry : metrics_) {
            const GameplayMetricKind kind = entry.second.kind;
            entry.second = Metric{};
            entry.second.kind = kind;
        }
    }

    std::string to_json() const override {
        const std::vector<GameplayMetric> metrics = snapshot();
        std::ostringstream out;
        out << "{\"metrics\":[";
        for (std::size_t i = 0; i < metrics.size(); ++i) {
            if (i != 0) out << ",";
            const GameplayMetric& metric = metrics[i];
            out << "{\"name\":\"" << json_escape(metric.name)
                << "\",\"kind\":\"" << kind_name(metric.kind)
                << "\",\"value\":" << emit_double(metric.value)
                << ",\"count\":" << metric.count
                << ",\"min\":" << emit_double(metric.minValue)
                << ",\"max\":" << emit_double(metric.maxValue) << "}";
        }
        out << "]}";
        return out.str();
    }

private:
    std::map<std::string, Metric> metrics_;
};

}  // namespace

std::unique_ptr<IGameplayMetrics> create_gameplay_metrics() {
    return std::make_unique<GameplayMetrics>();
}

}  // namespace gameplay
}  // namespace engine
