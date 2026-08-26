// DayNightCycle.cpp — the only TU implementing the public day/night cycle
// contract (Agente 4 §3 item 37 CORE): a deterministic clock advanced by
// dt (never wall time). Pure std + RegistryJson. time_of_day in [0,1);
// sun_altitude = sin(2*pi*(t - 0.25)) in [-1,1]; daylight_factor is a
// deterministic smoothstep of the altitude.

#include "engine/gameplay/IDayNightCycle.hpp"

#include "RegistryJson.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace engine {
namespace gameplay {
namespace {

constexpr float kTwoPi = 6.2831853071795864769f;

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

float wrap01(float value) {
    value -= std::floor(value);
    return value;
}

// smoothstep determinístico (Hermite) sobre o intervalo [edge0, edge1].
float smoothstep(float edge0, float edge1, float x) {
    const float t = (x - edge0) / (edge1 - edge0);
    const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return clamped * clamped * (3.0f - 2.0f * clamped);
}

class DayNightCycle final : public IDayNightCycle {
public:
    DayNightCycle() = default;

    bool configure(const DayNightConfig& config, std::string& errorOut) override {
        if (!finite_float(config.dayLengthSeconds) || config.dayLengthSeconds <= 0.0f) {
            errorOut = "day/night cycle: dayLengthSeconds must be finite and > 0";
            return false;
        }
        if (!finite_float(config.startOfDay) || config.startOfDay < 0.0f ||
            config.startOfDay >= 1.0f) {
            errorOut = "day/night cycle: startOfDay must be in [0,1)";
            return false;
        }
        config_ = config;
        elapsed_ = 0.0f;
        return true;
    }

    void advance(float dt) override {
        if (!(dt > 0.0f)) return;  // NaN/<=0 = no-op
        elapsed_ += dt;
    }

    void seek(float timeOfDay) override {
        if (!finite_float(timeOfDay)) return;
        const float target = wrap01(timeOfDay);
        const float current = time_of_day();
        // Desloca o relógio para atingir a fração alvo.
        elapsed_ += (target - current) * config_.dayLengthSeconds;
    }

    float time_of_day() const override {
        return wrap01(config_.startOfDay + elapsed_ / config_.dayLengthSeconds);
    }

    float sun_altitude() const override {
        return std::sin(kTwoPi * (time_of_day() - 0.25f));
    }

    float daylight_factor() const override {
        // Noite abaixo de -0.05; dia pleno acima de 0.35 (transição suave).
        return smoothstep(-0.05f, 0.35f, sun_altitude());
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"dayLengthSeconds\":" << config_.dayLengthSeconds
            << ",\"startOfDay\":" << config_.startOfDay
            << ",\"elapsed\":" << elapsed_ << "}";
        return out.str();
    }

    bool load_from_json(const std::string& jsonText,
                        std::string& errorOut) override {
        sdk::JsonValue root;
        if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
            if (errorOut.empty()) errorOut = "day/night cycle: root must be an object";
            return false;
        }
        DayNightConfig config;
        config.dayLengthSeconds =
            static_cast<float>(sdk::json_number(root, "dayLengthSeconds", 0.0));
        config.startOfDay =
            static_cast<float>(sdk::json_number(root, "startOfDay", 0.0));
        if (!finite_float(config.dayLengthSeconds) || config.dayLengthSeconds <= 0.0f) {
            errorOut = "day/night cycle: invalid dayLengthSeconds";
            return false;
        }
        if (!finite_float(config.startOfDay) || config.startOfDay < 0.0f ||
            config.startOfDay >= 1.0f) {
            errorOut = "day/night cycle: invalid startOfDay";
            return false;
        }
        config_ = config;
        elapsed_ = static_cast<float>(sdk::json_number(root, "elapsed", 0.0));
        if (!finite_float(elapsed_)) elapsed_ = 0.0f;
        return true;
    }

private:
    DayNightConfig config_{};
    float elapsed_{ 0.0f };
};

}  // namespace

std::unique_ptr<IDayNightCycle> create_day_night_cycle() {
    return std::make_unique<DayNightCycle>();
}

}  // namespace gameplay
}  // namespace engine
