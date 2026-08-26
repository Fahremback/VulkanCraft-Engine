#include "engine/simulation/IFixedTickSim.hpp"

#include "RegistryJson.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace engine {
namespace simulation {
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

bool FixedTickSimSpec::validate(std::string& errorOut) const {
    if (!finite(fixed_dt) || fixed_dt <= 0.0) {
        errorOut = "fixed_dt must be finite and > 0";
        return false;
    }
    if (max_ticks_per_frame < 1) {
        errorOut = "max_ticks_per_frame must be >= 1";
        return false;
    }
    errorOut.clear();
    return true;
}

bool FixedTickSimSpec::load_from_json(const std::string& json,
                                      std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "fixed tick spec must be an object";
        return false;
    }
    FixedTickSimSpec candidate;
    if (!number_field(doc, "fixed_dt", candidate.fixed_dt, false, errorOut)) {
        return false;
    }
    if (!int_field(doc, "max_ticks_per_frame", candidate.max_ticks_per_frame,
                   errorOut)) {
        return false;
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = candidate;
    return true;
}

std::string FixedTickSimSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"fixed_dt\":" << fixed_dt
        << ",\"max_ticks_per_frame\":" << max_ticks_per_frame << "}";
    return out.str();
}

namespace {

class FixedTickSim final : public IFixedTickSim {
public:
    bool configure(const FixedTickSimSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        accumulator_ = 0.0;
        return true;
    }

    FixedTickResult advance(double real_dt, std::string& errorOut) override {
        if (!finite(real_dt) || real_dt < 0.0) {
            errorOut = "real_dt must be finite and >= 0";
            return FixedTickResult{};
        }
        accumulator_ += real_dt;
        // Budget anti spiral-of-death: a dívida além do cap é DESCARTADA
        // (clamp do accumulator), nunca carregada para frames futuros.
        const double max_acc =
            static_cast<double>(spec_.max_ticks_per_frame) * spec_.fixed_dt;
        if (accumulator_ > max_acc) {
            accumulator_ = max_acc;
        }
        int ticks = 0;
        while (accumulator_ >= spec_.fixed_dt && ticks < spec_.max_ticks_per_frame) {
            accumulator_ -= spec_.fixed_dt;
            ++ticks;
        }
        if (accumulator_ < 1e-12) {
            accumulator_ = 0.0;  // higiene de ponto flutuante (resíduo ~0)
        }
        FixedTickResult result;
        result.ticks = ticks;
        result.alpha = accumulator_ / spec_.fixed_dt;
        errorOut.clear();
        return result;
    }

    double alpha() const override { return accumulator_ / spec_.fixed_dt; }

    double accumulator() const override { return accumulator_; }

    void reset() override { accumulator_ = 0.0; }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"accumulator\":" << accumulator_ << "}";
        return out.str();
    }

    bool deserialize_state(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) {
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "fixed tick state must be an object";
            return false;
        }
        const sdk::JsonValue* acc = doc.field("accumulator");
        if (acc == nullptr || acc->kind != sdk::JsonValue::Kind::Number ||
            !finite(acc->number) || acc->number < 0.0) {
            errorOut = "accumulator must be finite and >= 0";
            return false;
        }
        accumulator_ = acc->number;
        errorOut.clear();
        return true;
    }

private:
    FixedTickSimSpec spec_;
    double accumulator_ = 0.0;
};

}  // namespace

std::unique_ptr<IFixedTickSim> create_fixed_tick_sim() {
    return std::make_unique<FixedTickSim>();
}

}  // namespace simulation
}  // namespace engine
