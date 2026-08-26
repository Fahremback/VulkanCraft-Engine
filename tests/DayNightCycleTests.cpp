// DayNightCycleTests — gate do contrato IDayNightCycle (§3 item 37, day/night
// CORE): prova determinismo (mesmo dt → mesmo estado), wrap em [0,1), busca
// determinística, altitude (0 = meia-noite, pico ao meio-dia), daylight
// monotônico, recusas de config e round-trip JSON bit-exact.

#include "engine/gameplay/IDayNightCycle.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

bool near(float a, float b, float eps = 1.0e-4f) { return (a > b - eps) && (a < b + eps); }

void test_determinism_and_advance() {
    auto a = engine::gameplay::create_day_night_cycle();
    auto b = engine::gameplay::create_day_night_cycle();
    std::string error;
    engine::gameplay::DayNightConfig config;
    config.dayLengthSeconds = 100.0f;  // ciclo de 100s
    config.startOfDay = 0.0f;
    check(a->configure(config, error) && b->configure(config, error), "configure");

    check(near(a->time_of_day(), 0.0f), "t=0 é 0.0 (meia-noite)");
    a->advance(25.0f);
    check(near(a->time_of_day(), 0.25f), "25s de 100s → 0.25 (nascer)");
    check(near(a->sun_altitude(), 0.0f), "nascer: altitude 0");
    a->advance(25.0f);
    check(near(a->time_of_day(), 0.5f), "50s → 0.5 (meio-dia)");
    check(near(a->sun_altitude(), 1.0f), "meio-dia: altitude máxima");

    // Wrap: 100s total → volta a 0.
    a->advance(50.0f);
    check(near(a->time_of_day(), 0.0f), "wrap em 100s → 0.0");

    // Determinismo: b recebe a MESMA sequência completa (pré + dt) que a.
    const float pre[] = { 25.0f, 25.0f, 50.0f };
    for (const float dt : pre) b->advance(dt);
    const float dts[] = { 3.7f, 12.3f, 0.5f, 45.0f, 8.9f };
    for (const float dt : dts) {
        a->advance(dt);
        b->advance(dt);
    }
    check(a->to_json() == b->to_json(), "estado idêntico (mesma sequência de dt)");
    check(near(a->time_of_day(), b->time_of_day()), "time_of_day idêntico");

    // dt inválido é no-op.
    const std::string before = a->to_json();
    a->advance(-1.0f);
    a->advance(std::nan(""));
    check(a->to_json() == before, "dt negativo/NaN é no-op");
}

void test_daylight() {
    auto cycle = engine::gameplay::create_day_night_cycle();
    std::string error;
    engine::gameplay::DayNightConfig config;
    config.dayLengthSeconds = 100.0f;
    check(cycle->configure(config, error), "configure");

    cycle->seek(0.0f);  // meia-noite
    const float night = cycle->daylight_factor();
    cycle->seek(0.5f);  // meio-dia
    const float noon = cycle->daylight_factor();
    cycle->seek(0.75f);  // pôr do sol
    const float dusk = cycle->daylight_factor();
    check(night == 0.0f, "meia-noite: daylight 0");
    check(noon == 1.0f, "meio-dia: daylight 1");
    check(dusk > 0.0f && dusk < 1.0f, "pôr: transição intermediária");
    check(noon > dusk && dusk > night, "daylight monotônico com a altitude");
}

void test_seek_and_json() {
    auto cycle = engine::gameplay::create_day_night_cycle();
    std::string error;
    engine::gameplay::DayNightConfig config;
    config.dayLengthSeconds = 400.0f;
    config.startOfDay = 0.25f;
    check(cycle->configure(config, error), "configure startOfDay 0.25");
    check(near(cycle->time_of_day(), 0.25f), "começa em startOfDay");

    cycle->seek(0.9f);
    check(near(cycle->time_of_day(), 0.9f), "seek 0.9");
    cycle->seek(1.3f);
    check(near(cycle->time_of_day(), 0.3f), "seek wrap (1.3 → 0.3)");

    const std::string json = cycle->to_json();
    auto restored = engine::gameplay::create_day_night_cycle();
    check(restored->load_from_json(json, error), "load do estado");
    check(restored->to_json() == json, "round-trip bit-exact");
    check(near(restored->time_of_day(), 0.3f), "estado restaurado (0.3)");
}

void test_config_refusals() {
    auto cycle = engine::gameplay::create_day_night_cycle();
    std::string error;
    engine::gameplay::DayNightConfig config;
    config.dayLengthSeconds = 0.0f;
    check(!cycle->configure(config, error), "dayLength 0 recusa");
    config.dayLengthSeconds = 100.0f;
    config.startOfDay = -0.5f;
    check(!cycle->configure(config, error), "startOfDay negativa recusa");
    config.startOfDay = 1.5f;
    check(!cycle->configure(config, error), "startOfDay >= 1 recusa");
    config.startOfDay = 0.5f;
    check(cycle->configure(config, error), "config válida aceita");
    check(near(cycle->time_of_day(), 0.5f), "estado após config válida");
}

}  // namespace

int main() {
    test_determinism_and_advance();
    test_daylight();
    test_seek_and_json();
    test_config_refusals();

    if (failures == 0) {
        std::printf("day_night_cycle_tests: all checks passed\n");
        return 0;
    }
    std::printf("day_night_cycle_tests: %d failure(s)\n", failures);
    return 1;
}
