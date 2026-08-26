// AdaptiveMusicTests — gate do contrato público de música adaptativa (agente 4
// §7 item 75). Prova que camadas/estados/crossfade/stingers/intensidade são
// determinísticos, all-or-nothing no load, bit-exact no round-trip JSON, e se
// comportam como documentado.

#include "engine/audio/IAdaptiveMusic.hpp"

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

bool approx(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

using engine::audio::AdaptiveMusicSpec;
using engine::audio::IAdaptiveMusic;
using engine::audio::MusicEvent;
using engine::audio::MusicEventKind;
using engine::audio::MusicLayer;
using engine::audio::MusicState;
using engine::audio::MusicStinger;
using engine::audio::create_adaptive_music;

AdaptiveMusicSpec make_spec() {
    AdaptiveMusicSpec spec;
    spec.layers = {{"drums"}, {"bass"}, {"melody"}};
    spec.states = {
        {"explore", {{"drums", 0.3}, {"bass", 0.4}, {"melody", 0.8}}, 1.0},
        {"combat", {{"drums", 1.0}, {"bass", 0.9}, {"melody", 0.5}}, 2.0},
        {"quiet", {{"melody", 0.5}}, 0.0},
    };
    spec.stingers = {
        {"hit", "drums", 1.0},
        {"boss", "melody", 0.7},
    };
    return spec;
}

void test_spec_validate() {
    AdaptiveMusicSpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    AdaptiveMusicSpec bad = s;
    bad.layers[1].id = "drums";  // duplicado
    check(!bad.validate(err) && !err.empty(), "layer duplicada recusa");

    bad = s;
    bad.states[0].layer_gains[0].first = "nope";
    check(!bad.validate(err) && !err.empty(), "state layer desconhecida recusa");

    bad = s;
    bad.states[0].layer_gains[0].second = 1.5;
    check(!bad.validate(err) && !err.empty(), "gain > 1 recusa");

    bad = s;
    bad.states[1].transition_s = -1.0;
    check(!bad.validate(err) && !err.empty(), "transition_s negativo recusa");

    bad = s;
    bad.states[2].id = "explore";  // duplicado
    check(!bad.validate(err) && !err.empty(), "state id duplicado recusa");

    bad = s;
    bad.stingers[0].layer = "nope";
    check(!bad.validate(err) && !err.empty(), "stinger layer desconhecida recusa");

    bad = s;
    bad.stingers[1].id = "hit";  // duplicado
    check(!bad.validate(err) && !err.empty(), "stinger id duplicado recusa");
}

void test_spec_json_roundtrip() {
    const AdaptiveMusicSpec spec = make_spec();
    const std::string json = spec.to_json();
    AdaptiveMusicSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    AdaptiveMusicSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
    check(!loaded.load_from_json("{\"version\":2,\"layers\":[]}", err) &&
              !err.empty(),
          "versão desconhecida recusa");
}

void test_instant_transition() {
    auto m = create_adaptive_music();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->current_state().empty(), "sem estado inicial");
    check(m->set_state("quiet", err), "set_state quiet (transition 0)");
    check(m->current_state() == "quiet", "estado atual = quiet (instantâneo)");
    check(approx(m->layer_gain("melody"), 0.5), "melody 0.5");
    check(approx(m->layer_gain("drums"), 0.0), "drums 0 (ausente no estado)");

    const auto events = m->drain_events();
    check(events.size() == 1 && events[0].kind == MusicEventKind::StateChanged &&
              events[0].id == "quiet",
          "transição instantânea emite StateChanged imediato");
}

void test_crossfade() {
    auto m = create_adaptive_music();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->set_state("explore", err), "set_state explore (fade 1s)");
    check(m->current_state().empty(), "fade pendente: estado ainda vazio");
    check(m->tick(1.0, err), "tick 1s completa o fade do explore");
    m->drain_events();  // descarta o StateChanged do explore
    check(m->current_state() == "explore", "estado atual = explore");

    check(m->set_state("combat", err), "set_state combat (crossfade 2s)");
    check(m->current_state() == "explore", "durante o fade o estado ainda é explore");

    check(m->tick(1.0, err), "tick 1s (metade do fade)");
    check(m->current_state() == "explore", "metade do fade: ainda explore");
    check(approx(m->layer_gain("drums"), (0.3 + 1.0) * 0.5),  // midpoint
          "drums no meio do fade = média");
    check(approx(m->layer_gain("bass"), (0.4 + 0.9) * 0.5),
          "bass no meio do fade = média");
    check(m->drain_events().empty(), "nenhum evento no meio do fade");

    check(m->tick(1.0, err), "tick +1s (fade completo)");
    check(m->current_state() == "combat", "estado atual = combat");
    check(approx(m->layer_gain("drums"), 1.0) && approx(m->layer_gain("bass"), 0.9) &&
              approx(m->layer_gain("melody"), 0.5),
          "ganhos finais do combat");
    const auto events = m->drain_events();
    check(events.size() == 1 && events[0].kind == MusicEventKind::StateChanged &&
              events[0].id == "combat",
          "StateChanged emitido ao completar o fade");
}

void test_intensity() {
    auto m = create_adaptive_music();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->set_state("quiet", err), "set_state quiet");
    m->drain_events();
    check(approx(m->layer_gain("melody"), 0.5), "intensidade 1: ganho pleno");
    check(m->set_intensity(0.5, err), "set_intensity 0.5");
    check(approx(m->layer_gain("melody"), 0.25), "intensidade 0.5: ganho pela metade");
    check(!m->set_intensity(1.5, err) && !err.empty(), "intensity > 1 recusa");
}

void test_stingers() {
    auto m = create_adaptive_music();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->trigger_stinger("hit", err), "trigger hit");
    check(m->trigger_stinger("boss", err), "trigger boss");
    check(!m->trigger_stinger("nope", err) && !err.empty(), "stinger desconhecido recusa");

    const auto events = m->drain_events();
    check(events.size() == 2, "dois eventos de stinger");
    check(events[0].kind == MusicEventKind::StingerTriggered && events[0].id == "hit" &&
              events[0].layer == "drums" && approx(events[0].intensity, 1.0),
          "stinger hit: layer drums, intensity 1");
    check(events[1].id == "boss" && events[1].layer == "melody" &&
              approx(events[1].intensity, 0.7),
          "stinger boss: layer melody, intensity 0.7");
    check(m->drain_events().empty(), "fila esvaziada após drain");
}

void test_set_state_noop_and_unknown() {
    auto m = create_adaptive_music();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->set_state("explore", err), "set_state explore");
    m->drain_events();
    check(m->set_state("explore", err), "set_state explore de novo (no-op)");
    check(m->drain_events().empty(), "no-op não emite evento");
    check(!m->set_state("nope", err) && !err.empty(), "state desconhecido recusa");
    check(approx(m->layer_gain("nope"), 0.0), "layer_gain desconhecida = 0");
}

void test_state_roundtrip() {
    auto a = create_adaptive_music();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    check(a->set_state("combat", err), "set_state combat");
    a->tick(2.0, err);  // completa o fade
    a->drain_events();
    check(a->set_intensity(0.75, err), "intensity 0.75");

    const std::string state = a->serialize_state();
    auto b = create_adaptive_music();
    check(b->configure(make_spec(), err), "configure b");
    check(b->deserialize_state(state, err) && err.empty(), "deserialize state");
    check(b->serialize_state() == state, "state round-trip bit-exact");
    check(b->current_state() == "combat", "estado restaurado");
    check(approx(b->layer_gain("drums"), 0.75),  // 1.0 * 0.75
          "ganho com intensidade restaurado");

    auto c = create_adaptive_music();
    check(c->configure(make_spec(), err), "configure c");
    const std::string before = c->serialize_state();
    check(!c->deserialize_state(
              "{\"current_state\":\"nope\",\"intensity\":1,\"gains\":{}}", err) &&
              !err.empty(),
          "current_state desconhecido recusa (all-or-nothing)");
    check(c->serialize_state() == before, "recusa não muta (all-or-nothing)");
    check(!c->deserialize_state(
              "{\"current_state\":\"explore\",\"intensity\":1,"
              "\"gains\":{\"nope\":0.5}}",
              err) &&
              !err.empty(),
          "gains com layer desconhecida recusa");
}

void test_determinism() {
    auto a = create_adaptive_music();
    auto b = create_adaptive_music();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    check(b->configure(make_spec(), err), "configure b");
    for (int i = 0; i < 4; ++i) {
        a->set_state((i % 2 == 0) ? "explore" : "combat", err);
        b->set_state((i % 2 == 0) ? "explore" : "combat", err);
        a->tick(0.5, err);
        b->tick(0.5, err);
        if (i == 1) {
            a->trigger_stinger("hit", err);
            b->trigger_stinger("hit", err);
        }
    }
    a->tick(2.0, err);
    b->tick(2.0, err);
    check(a->serialize_state() == b->serialize_state(),
          "determinismo: estado bit-exato");
    const auto ea = a->drain_events();
    const auto eb = b->drain_events();
    check(ea.size() == eb.size(), "determinismo: mesma quantidade de eventos");
    for (std::size_t i = 0; i < ea.size() && i < eb.size(); ++i) {
        check(ea[i].kind == eb[i].kind && ea[i].id == eb[i].id &&
                  ea[i].layer == eb[i].layer &&
                  ea[i].intensity == eb[i].intensity,
              "determinismo: eventos idênticos em ordem");
    }
}

}  // namespace

int main() {
    test_spec_validate();
    test_spec_json_roundtrip();
    test_instant_transition();
    test_crossfade();
    test_intensity();
    test_stingers();
    test_set_state_noop_and_unknown();
    test_state_roundtrip();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "adaptive_music_tests: all checks passed\n";
    } else {
        std::cout << "adaptive_music_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
