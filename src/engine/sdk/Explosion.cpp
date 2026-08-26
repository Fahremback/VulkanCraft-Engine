// Explosion.cpp — adapter único de IExplosion (engine::physics).
// Falloff (1 − d/radius)^power; fragmentos via xorshift32 determinístico
// encadeado ao seed (direção em esfera unitária, speed escalada, massa
// uniforme). JSON bit-exact all-or-nothing.

#include "engine/physics/IExplosion.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace engine::physics {
namespace {

constexpr double kPi = 3.14159265358979323846;

class Explosion final : public IExplosion {
public:
    bool configure(const ExplosionSpec& spec, std::string& errorOut) override {
        if (!std::isfinite(spec.radius) || spec.radius <= 0.0) {
            errorOut = "radius must be finite and > 0";
            return false;
        }
        if (!std::isfinite(spec.impulse) || spec.impulse < 0.0 ||
            !std::isfinite(spec.heat) || spec.heat < 0.0 ||
            !std::isfinite(spec.damage) || spec.damage < 0.0) {
            errorOut = "impulse/heat/damage must be finite and >= 0";
            return false;
        }
        if (!std::isfinite(spec.falloff_power) || spec.falloff_power < 0.0) {
            errorOut = "falloff_power must be finite and >= 0";
            return false;
        }
        if (spec.fragments <= 0) {
            errorOut = "fragments must be > 0";
            return false;
        }
        if (!std::isfinite(spec.fragment_speed) || spec.fragment_speed < 0.0) {
            errorOut = "fragment_speed must be finite and >= 0";
            return false;
        }
        spec_ = spec;
        errorOut.clear();
        return true;
    }

    const ExplosionSpec& spec() const override { return spec_; }

    ExplosionSample sample_at(double distance,
                              std::string& errorOut) const override {
        ExplosionSample out;
        if (!std::isfinite(distance)) {
            errorOut = "distance must be finite";
            return out;
        }
        if (distance < 0.0) {
            errorOut = "distance must be >= 0";
            return out;
        }
        out.distance = distance;
        if (distance >= spec_.radius) {
            out.falloff = 0.0;
            return out;
        }
        out.falloff = std::pow(1.0 - distance / spec_.radius,
                               spec_.falloff_power);
        out.impulse = spec_.impulse * out.falloff;
        out.heat = spec_.heat * out.falloff;
        out.damage = spec_.damage * out.falloff;
        errorOut.clear();
        return out;
    }

    std::vector<ExplosionFragment> fragments(
        uint64_t seed, std::string& errorOut) const override {
        std::vector<ExplosionFragment> out;
        out.reserve(static_cast<std::size_t>(spec_.fragments));
        uint32_t state =
            static_cast<uint32_t>(seed ^ 0x9E3779B9u);
        if (state == 0u) state = 0x853C49E1u;
        const double inv = 1.0 / 65535.0;
        const double mass = 1.0 / static_cast<double>(spec_.fragments);
        for (int i = 0; i < spec_.fragments; ++i) {
            auto next = [&state]() -> uint32_t {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                return state;
            };
            const double u = static_cast<double>(next() & 0xFFFFu) * inv;
            const double v = static_cast<double>(next() & 0xFFFFu) * inv;
            const double w = static_cast<double>(next() & 0xFFFFu) * inv;
            const double theta = std::acos(2.0 * u - 1.0);
            const double phi = 2.0 * kPi * v;
            const double st = std::sin(theta);
            ExplosionFragment f;
            f.direction = {st * std::cos(phi), std::cos(theta),
                           st * std::sin(phi)};
            f.speed = spec_.fragment_speed * (0.5 + 0.5 * w);
            f.mass = mass;
            out.push_back(f);
        }
        errorOut.clear();
        return out;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"radius\":" << spec_.radius
            << ",\"impulse\":" << spec_.impulse
            << ",\"heat\":" << spec_.heat
            << ",\"damage\":" << spec_.damage
            << ",\"falloff_power\":" << spec_.falloff_power
            << ",\"fragments\":" << spec_.fragments
            << ",\"fragment_speed\":" << spec_.fragment_speed << "}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) return false;
        if (!doc.is_object()) {
            errorOut = "explosion state must be an object";
            return false;
        }
        ExplosionSpec s;
        const sdk::JsonValue* radius = doc.field("radius");
        const sdk::JsonValue* impulse = doc.field("impulse");
        const sdk::JsonValue* heat = doc.field("heat");
        const sdk::JsonValue* damage = doc.field("damage");
        const sdk::JsonValue* power = doc.field("falloff_power");
        const sdk::JsonValue* fragments = doc.field("fragments");
        const sdk::JsonValue* speed = doc.field("fragment_speed");
        if (radius == nullptr || impulse == nullptr || heat == nullptr ||
            damage == nullptr || power == nullptr || fragments == nullptr ||
            speed == nullptr ||
            radius->kind != sdk::JsonValue::Kind::Number ||
            impulse->kind != sdk::JsonValue::Kind::Number ||
            heat->kind != sdk::JsonValue::Kind::Number ||
            damage->kind != sdk::JsonValue::Kind::Number ||
            power->kind != sdk::JsonValue::Kind::Number ||
            fragments->kind != sdk::JsonValue::Kind::Number ||
            speed->kind != sdk::JsonValue::Kind::Number) {
            errorOut = "explosion state malformed";
            return false;
        }
        s.radius = radius->number;
        s.impulse = impulse->number;
        s.heat = heat->number;
        s.damage = damage->number;
        s.falloff_power = power->number;
        s.fragments = static_cast<int>(fragments->number);
        s.fragment_speed = speed->number;
        if (!validate(s, errorOut)) return false;
        spec_ = s;
        errorOut.clear();
        return true;
    }

private:
    static bool validate(const ExplosionSpec& s, std::string& errorOut) {
        if (!std::isfinite(s.radius) || s.radius <= 0.0) {
            errorOut = "radius must be finite and > 0";
            return false;
        }
        if (!std::isfinite(s.impulse) || s.impulse < 0.0 ||
            !std::isfinite(s.heat) || s.heat < 0.0 ||
            !std::isfinite(s.damage) || s.damage < 0.0) {
            errorOut = "impulse/heat/damage must be finite and >= 0";
            return false;
        }
        if (!std::isfinite(s.falloff_power) || s.falloff_power < 0.0) {
            errorOut = "falloff_power must be finite and >= 0";
            return false;
        }
        if (s.fragments <= 0) {
            errorOut = "fragments must be > 0";
            return false;
        }
        if (!std::isfinite(s.fragment_speed) || s.fragment_speed < 0.0) {
            errorOut = "fragment_speed must be finite and >= 0";
            return false;
        }
        return true;
    }

    ExplosionSpec spec_;
};

}  // namespace

std::unique_ptr<IExplosion> create_explosion() {
    return std::unique_ptr<IExplosion>(new Explosion());
}

}  // namespace engine::physics
