// FixedTickSimTests — gate do contrato público de fixed timestep (agente 4 §1
// item 19). Prova que o acumulador é determinístico, all-or-nothing no load,
// bit-exact no round-trip, e que ticks/alpha/budget/reset se comportam como
// documentado — independente do FPS.

#include "engine/simulation/IFixedTickSim.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

using engine::simulation::FixedTickSimSpec;
using engine::simulation::create_fixed_tick_sim;

const double kDt = 1.0 / 60.0;

void test_validate_all_or_nothing() {
    FixedTickSimSpec s;
    std::string err;
    check(s.validate(err) && err.empty(), "spec default aceita");

    FixedTickSimSpec bad = s;
    bad.fixed_dt = 0.0;
    check(!bad.validate(err) && !err.empty(), "fixed_dt <= 0 recusa");
    bad = s;
    bad.fixed_dt = std::nan("");
    check(!bad.validate(err) && !err.empty(), "fixed_dt NaN recusa");
    bad = s;
    bad.max_ticks_per_frame = 0;
    check(!bad.validate(err) && !err.empty(), "max_ticks_per_frame < 1 recusa");
}

void test_spec_json_roundtrip() {
    FixedTickSimSpec spec;
    spec.fixed_dt = 0.02;
    spec.max_ticks_per_frame = 4;
    const std::string json = spec.to_json();
    FixedTickSimSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    FixedTickSimSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta");
}

void test_basic_advance() {
    auto sim = create_fixed_tick_sim();
    FixedTickSimSpec spec;
    spec.fixed_dt = kDt;
    std::string err;
    check(sim->configure(spec, err), "configure");

    // real_dt < fixed_dt → 0 ticks, alpha = real_dt/fixed_dt
    auto r = sim->advance(kDt * 0.5, err);
    check(r.ticks == 0 && std::fabs(r.alpha - 0.5) < 1e-12,
          "meio tick → 0 ticks, alpha 0.5");

    // exatamente um tick
    r = sim->advance(kDt * 0.5, err);  // acumula 1.0 tick
    check(r.ticks == 1 && r.alpha == 0.0, "acumula até 1 tick, alpha 0");

    // 2.5 ticks → 2 ticks, alpha 0.5
    r = sim->advance(kDt * 2.5, err);
    check(r.ticks == 2 && std::fabs(r.alpha - 0.5) < 1e-12,
          "2.5 ticks → 2 ticks, alpha 0.5");
}

void test_accumulation_across_frames() {
    auto sim = create_fixed_tick_sim();
    FixedTickSimSpec spec;
    spec.fixed_dt = kDt;
    std::string err;
    check(sim->configure(spec, err), "configure");

    // três frames de 0.5 tick = 1.5 ticks → 1 tick no último, alpha 0.5
    check(sim->advance(kDt * 0.5, err).ticks == 0, "frame 1: 0 ticks");
    check(sim->advance(kDt * 0.5, err).ticks == 1, "frame 2: 1 tick (acumulado)");
    auto r = sim->advance(kDt * 0.5, err);
    check(r.ticks == 0 && std::fabs(r.alpha - 0.5) < 1e-12,
          "frame 3: 0 ticks, alpha 0.5 (residual preservado)");
}

void test_max_ticks_budget() {
    auto sim = create_fixed_tick_sim();
    FixedTickSimSpec spec;
    spec.fixed_dt = kDt;
    spec.max_ticks_per_frame = 3;
    std::string err;
    check(sim->configure(spec, err), "configure");

    // frame gigante (10 ticks) → cap de 3; a dívida além do cap é DESCARTADA
    // (clamp — anti spiral-of-death), o accumulator volta para 0.
    auto r = sim->advance(kDt * 10.0, err);
    check(r.ticks == 3, "budget: máx 3 ticks por frame");
    check(sim->accumulator() == 0.0, "excesso além do cap descartado (clamp)");
    check(sim->advance(0.0, err).ticks == 0, "sem dívida carregada p/ o próximo frame");
    check(sim->advance(0.0, err).ticks == 0, "frames seguintes seguem limpos");
}

void test_alpha_range_and_reset() {
    auto sim = create_fixed_tick_sim();
    FixedTickSimSpec spec;
    spec.fixed_dt = kDt;
    std::string err;
    check(sim->configure(spec, err), "configure");

    for (int i = 0; i < 200; ++i) {
        auto r = sim->advance(1.0 / 137.0, err);  // FPS estranho
        check(r.alpha >= 0.0 && r.alpha < 1.0, "alpha sempre em [0,1)");
    }
    check(sim->advance(kDt * 10.0, err).ticks >= 1, "dívida acumulada drena");

    sim->reset();
    check(sim->accumulator() == 0.0, "reset zera o accumulator");
}

void test_state_roundtrip() {
    auto sim = create_fixed_tick_sim();
    FixedTickSimSpec spec;
    spec.fixed_dt = kDt;
    std::string err;
    check(sim->configure(spec, err), "configure");
    sim->advance(kDt * 1.5, err);  // 1 tick, residual 0.5

    const std::string state = sim->serialize_state();
    auto other = create_fixed_tick_sim();
    check(other->configure(spec, err), "configure other");
    check(other->deserialize_state(state, err) && err.empty(), "deserialize");
    check(other->serialize_state() == state, "state round-trip bit-exact");
    auto r = other->advance(0.0, err);
    // %.9g perde alguns ulps no round-trip → tolerância 1e-9 (não 1e-12).
    check(r.ticks == 0 && std::fabs(r.alpha - 0.5) < 1e-9,
          "estado restaurado reproduz o próximo advance");

    check(!other->deserialize_state("{bad", err) && !err.empty(), "estado inválido recusa");
    check(!other->deserialize_state("{\"accumulator\":-1}", err) && !err.empty(),
          "accumulator negativo recusa");
}

void test_determinism() {
    auto a = create_fixed_tick_sim();
    auto b = create_fixed_tick_sim();
    FixedTickSimSpec spec;
    spec.fixed_dt = kDt;
    spec.max_ticks_per_frame = 4;
    std::string err;
    check(a->configure(spec, err), "configure a");
    check(b->configure(spec, err), "configure b");

    // sequência de FPS irregular
    const double dts[] = {0.016, 0.033, 0.008, 0.050, 0.021, 0.012, 0.040};
    for (const double dt : dts) {
        const auto ra = a->advance(dt, err);
        const auto rb = b->advance(dt, err);
        check(ra.ticks == rb.ticks && ra.alpha == rb.alpha,
              "determinismo: ticks/alpha bit-exatos cross-instance");
    }
    check(a->serialize_state() == b->serialize_state(),
          "determinismo: estado bit-exato");
}

}  // namespace

int main() {
    test_validate_all_or_nothing();
    test_spec_json_roundtrip();
    test_basic_advance();
    test_accumulation_across_frames();
    test_max_ticks_budget();
    test_alpha_range_and_reset();
    test_state_roundtrip();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "fixed_tick_sim_tests: all checks passed\n";
    } else {
        std::cout << "fixed_tick_sim_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
