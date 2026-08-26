// AudioMixerTests — gate do contrato público de mixer de áudio data-driven
// (agente 4 §7 item 73). Prova que mixer/buses/roteamento são determinísticos,
// all-or-nothing no load, bit-exact no round-trip JSON, e que ganho em dB,
// sidechain ducking, snapshots e nível de master se comportam como documentado.

#include "engine/audio/IAudioMixer.hpp"

#include <cmath>
#include <iostream>
#include <limits>
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

bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

using engine::audio::AudioBus;
using engine::audio::AudioMixerSpec;
using engine::audio::AudioSidechain;
using engine::audio::AudioSnapshot;
using engine::audio::IAudioMixer;
using engine::audio::create_audio_mixer;
using engine::audio::db_to_linear;
using engine::audio::linear_to_db;

// master (root)
//   ├─ music
//   └─ sfx
// sidechain: sfx (source) ducka music (target) em até -12 dB acima de 0.5.
AudioMixerSpec make_spec() {
    AudioMixerSpec spec;
    spec.buses = {
        {"master", 0.0, ""},
        {"music", -3.0, "master"},
        {"sfx", 0.0, "master"},
    };
    spec.sidechains = {
        {"sfx", "music", 0.5, -12.0, 0.1, 0.5},
    };
    AudioSnapshot quiet;
    quiet.name = "quiet";
    quiet.gains = {{"music", -20.0}, {"sfx", 0.0}};
    AudioSnapshot full;
    full.name = "full";
    full.gains = {{"music", 0.0}};
    spec.snapshots = {quiet, full};
    return spec;
}

void test_db_conversions() {
    check(linear_to_db(1.0) == 0.0, "linear_to_db(1) = 0");
    check(db_to_linear(0.0) == 1.0, "db_to_linear(0) = 1");
    check(approx(db_to_linear(-6.0206), 0.5, 1e-4), "db_to_linear(-6.0206) ≈ 0.5");
    check(approx(linear_to_db(0.5), -6.0206, 1e-4), "linear_to_db(0.5) ≈ -6.0206");
    check(approx(db_to_linear(linear_to_db(0.25)), 0.25), "round-trip db→linear→db");
    check(std::isinf(linear_to_db(0.0)) && linear_to_db(0.0) < 0.0,
          "linear_to_db(0) = -inf");
    check(std::isinf(linear_to_db(-1.0)) && linear_to_db(-1.0) < 0.0,
          "linear_to_db(negativo) = -inf");
    check(db_to_linear(-std::numeric_limits<double>::infinity()) == 0.0,
          "db_to_linear(-inf) = 0");
}

void test_spec_validate() {
    AudioMixerSpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    AudioMixerSpec bad = s;
    bad.buses[1].id = "master";  // duplicado
    check(!bad.validate(err) && !err.empty(), "bus id duplicado recusa");

    bad = s;
    bad.buses[1].parent = "nope";
    check(!bad.validate(err) && !err.empty(), "parent desconhecido recusa");

    bad = s;
    bad.sidechains[0].target = "nope";
    check(!bad.validate(err) && !err.empty(), "sidechain target desconhecido recusa");

    bad = s;
    bad.sidechains[0].source = "nope";
    check(!bad.validate(err) && !err.empty(), "sidechain source desconhecido recusa");

    bad = s;
    bad.sidechains[0].threshold = 1.5;
    check(!bad.validate(err) && !err.empty(), "threshold > 1 recusa");

    bad = s;
    bad.snapshots[0].gains[0].bus = "nope";
    check(!bad.validate(err) && !err.empty(), "snapshot bus desconhecido recusa");

    bad = s;
    bad.snapshots[1].name = "quiet";  // duplicado
    check(!bad.validate(err) && !err.empty(), "snapshot nome duplicado recusa");

    bad = s;
    bad.buses[0].parent = "music";  // ciclo master → music → master
    check(!bad.validate(err) && !err.empty(), "ciclo de roteamento recusa");
}

void test_spec_json_roundtrip() {
    const AudioMixerSpec spec = make_spec();
    const std::string json = spec.to_json();
    AudioMixerSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    AudioMixerSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
    check(!loaded.load_from_json("{\"version\":2,\"buses\":[]}", err) && !err.empty(),
          "versão desconhecida recusa");
}

void test_levels_and_routing() {
    auto m = create_audio_mixer();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->set_input("music", 0.5, err), "set_input music");
    check(m->set_input("sfx", 0.0, err), "set_input sfx");

    check(m->gain_db("music") == -3.0, "gain_db music = base");
    check(m->gain_db("sfx") == 0.0, "gain_db sfx = base");

    const double music_level = 0.5 * db_to_linear(-3.0);
    check(approx(m->bus_level("music"), music_level), "bus_level = input*linear(gain)");
    check(approx(m->master_level(), music_level), "master = soma dos filhos da raiz");

    check(m->set_input("sfx", 0.25, err), "set_input sfx 0.25");
    const double sfx_level = 0.25 * db_to_linear(0.0);  // = 0.25
    check(approx(m->master_level(), music_level + sfx_level), "master soma music+sfx");
}

void test_sidechain_ducking() {
    auto m = create_audio_mixer();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->set_input("sfx", 1.0, err), "sfx acima do threshold");
    check(m->set_input("music", 0.0, err), "music input");

    check(m->gain_db("music") == -3.0, "sem duck no início");
    check(m->tick(0.05, err), "tick ataque 0.05");
    check(m->gain_db("music") == -9.0, "ataque parcial: -3 + (-6) = -9");
    check(m->tick(0.05, err), "tick ataque 0.05");
    check(m->gain_db("music") == -15.0, "ataque completo: -3 + (-12) = -15");
    check(m->tick(0.05, err), "tick extra");
    check(m->gain_db("music") == -15.0, "duck clampado em -12");

    check(m->set_input("sfx", 0.0, err), "sfx abaixo do threshold");
    check(m->tick(0.25, err), "tick release 0.25");
    check(m->gain_db("music") == -9.0, "release parcial: -3 + (-6) = -9");
    check(m->tick(0.25, err), "tick release 0.25");
    check(m->gain_db("music") == -3.0, "release completo: volta a -3");
}

void test_snapshot() {
    auto m = create_audio_mixer();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->apply_snapshot("quiet", err), "apply_snapshot quiet");
    check(m->gain_db("music") == -20.0 && m->gain_db("sfx") == 0.0,
          "snapshot quiet aplica ganhos");
    check(m->apply_snapshot("full", err), "apply_snapshot full");
    check(m->gain_db("music") == 0.0, "snapshot full restaura music");

    check(!m->apply_snapshot("nope", err) && !err.empty(), "snapshot desconhecido recusa");
    check(m->gain_db("music") == 0.0, "estado intacto após recusa de snapshot");
}

void test_state_roundtrip() {
    auto m = create_audio_mixer();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->set_input("sfx", 1.0, err), "sfx alto");
    check(m->tick(0.1, err), "tick (duck completo)");
    check(m->gain_db("music") == -15.0, "duck completo com base -3");
    check(m->apply_snapshot("quiet", err), "snapshot quiet");
    check(m->gain_db("music") == -32.0, "base -20 + duck -12 = -32");

    const std::string state = m->serialize_state();
    auto g = create_audio_mixer();
    check(g->configure(make_spec(), err), "configure g");
    check(g->deserialize_state(state, err) && err.empty(), "deserialize state");
    check(g->serialize_state() == state, "state round-trip bit-exact");
    check(g->gain_db("music") == m->gain_db("music"), "duck + ganho restaurados");

    auto h = create_audio_mixer();
    check(h->configure(make_spec(), err), "configure h");
    const std::string before = h->serialize_state();
    check(!h->deserialize_state("{\"duck\":{\"nope\":1},\"gains\":{}}", err) &&
              !err.empty(),
          "bus desconhecido recusa (all-or-nothing)");
    check(h->serialize_state() == before, "recusa não muta (all-or-nothing)");
}

void test_runtime_refusals() {
    auto m = create_audio_mixer();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(!m->set_input("nope", 0.5, err) && !err.empty(),
          "set_input bus desconhecido recusa");
    check(!m->tick(-1.0, err) && !err.empty(), "tick dt negativo recusa");
    check(!m->tick(std::numeric_limits<double>::quiet_NaN(), err) && !err.empty(),
          "tick dt NaN recusa");
    check(m->gain_db("nope") == 0.0, "gain_db bus desconhecido = 0");
    check(m->bus_level("nope") == 0.0, "bus_level bus desconhecido = 0");

    check(m->set_input("music", 0.9, err), "set music 0.9");
    check(m->configure(make_spec(), err), "reconfigure");
    check(approx(m->bus_level("music"), 0.0), "reconfigure zera inputs");
}

void test_master_clamp() {
    auto m = create_audio_mixer();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    m->set_input("music", 1.0, err);
    m->set_input("sfx", 1.0, err);
    check(m->master_level() == 1.0, "master clampado em [0,1]");
}

void test_determinism() {
    auto a = create_audio_mixer();
    auto b = create_audio_mixer();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    check(b->configure(make_spec(), err), "configure b");

    for (int i = 0; i < 20; ++i) {
        const double sfx = (i % 4 == 0) ? 1.0 : 0.0;
        a->set_input("sfx", sfx, err);
        b->set_input("sfx", sfx, err);
        a->set_input("music", 0.3, err);
        b->set_input("music", 0.3, err);
        a->tick(0.05, err);
        b->tick(0.05, err);
    }
    check(a->serialize_state() == b->serialize_state(), "determinismo: estado bit-exato");
    check(a->master_level() == b->master_level(), "determinismo: master bit-exato");
    check(a->gain_db("music") == b->gain_db("music"), "determinismo: gain bit-exato");
}

}  // namespace

int main() {
    test_db_conversions();
    test_spec_validate();
    test_spec_json_roundtrip();
    test_levels_and_routing();
    test_sidechain_ducking();
    test_snapshot();
    test_state_roundtrip();
    test_runtime_refusals();
    test_master_clamp();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "audio_mixer_tests: all checks passed\n";
    } else {
        std::cout << "audio_mixer_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
