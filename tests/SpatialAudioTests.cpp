// SpatialAudioTests — gate do contrato público de áudio espacial (agente 4
// §7 item 74). Prova que atenuação por distância, panning equal-power,
// oclusão, reverb zones e virtualização são determinísticos, all-or-nothing
// no load, bit-exact no round-trip JSON, e se comportam como documentado.

#include "engine/audio/ISpatialAudio.hpp"

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

using engine::audio::AudioReverbZone;
using engine::audio::AudioSpatialSpec;
using engine::audio::AudioSourceInput;
using engine::audio::AudioSourceResult;
using engine::audio::ISpatialAudio;
using engine::audio::RolloffModel;
using engine::audio::Vec3;
using engine::audio::create_spatial_audio;

AudioSpatialSpec make_spec() {
    AudioSpatialSpec spec;
    spec.min_distance = 2.0;
    spec.max_distance = 100.0;
    spec.rolloff = RolloffModel::Inverse;
    spec.master_gain_db = 0.0;
    spec.zones = {
        {"hall", {0.0, 0.0, 0.0}, {5.0, 3.0, 5.0}, 0.5, 0.4},
    };
    return spec;
}

void test_spec_validate() {
    AudioSpatialSpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    AudioSpatialSpec bad = s;
    bad.min_distance = 0.0;
    check(!bad.validate(err) && !err.empty(), "min_distance 0 recusa");

    bad = s;
    bad.max_distance = 1.0;  // <= min 2.0
    check(!bad.validate(err) && !err.empty(), "max <= min recusa");

    bad = s;
    bad.zones[0].half_extents.y = 0.0;
    check(!bad.validate(err) && !err.empty(), "half_extents 0 recusa");

    bad = s;
    bad.zones[0].wet = 1.5;
    check(!bad.validate(err) && !err.empty(), "wet > 1 recusa");

    bad = s;
    bad.zones.push_back(bad.zones[0]);  // duplicado
    check(!bad.validate(err) && !err.empty(), "zone id duplicado recusa");
}

void test_spec_json_roundtrip() {
    const AudioSpatialSpec spec = make_spec();
    const std::string json = spec.to_json();
    AudioSpatialSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    AudioSpatialSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
    check(!loaded.load_from_json(
              "{\"version\":2,\"min_distance\":1,\"max_distance\":10}", err) &&
              !err.empty(),
          "versão desconhecida recusa");
    check(!loaded.load_from_json(
              "{\"version\":1,\"min_distance\":1,\"max_distance\":10,"
              "\"rolloff\":\"quadratic\"}",
              err) &&
              !err.empty(),
          "rolloff desconhecido recusa");
}

void test_front_center_no_nan() {
    auto a = create_spatial_audio();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    check(a->set_listener({0, 0, 0}, {1, 0, 0}, err), "listener");
    check(a->set_source("s", {{2, 0, 0}, 0.0, 0.5, 0.0, true}, err), "fonte frontal");
    check(a->set_source("here", {{0, 0, 0}, 0.0, 0.5, 0.0, true}, err),
          "fonte no listener (d=0)");
    check(a->update(err), "update");

    const double c = std::sqrt(0.5);  // pan central equal-power
    const AudioSourceResult front = a->source_result("s");
    check(approx(front.gain_l, c) && approx(front.gain_r, c),
          "frontal: L=R=√0.5 (atten=1, pan=0)");
    check(!front.virtualized, "frontal não virtualizada");

    const AudioSourceResult here = a->source_result("here");
    check(std::isfinite(here.gain_l) && std::isfinite(here.gain_r),
          "d=0 nunca produz NaN");
    check(approx(here.gain_l, c) && approx(here.gain_r, c),
          "d=0: attenu=1, pan central");
}

void test_rolloff_models() {
    // Linear: min 2, max 10.
    {
        AudioSpatialSpec spec = make_spec();
        spec.rolloff = RolloffModel::Linear;
        spec.max_distance = 10.0;
        auto a = create_spatial_audio();
        std::string err;
        check(a->configure(spec, err), "configure linear");
        check(a->set_listener({0, 0, 0}, {1, 0, 0}, err), "listener");
        a->set_source("mid", {{6, 0, 0}, 0.0, 0.5, 0.0, true}, err);   // t=0.5
        a->set_source("far", {{10, 0, 0}, 0.0, 0.5, 0.0, true}, err);  // t=1
        a->set_source("near", {{2, 0, 0}, 0.0, 0.5, 0.0, true}, err);  // t=0
        a->update(err);
        const double c = std::sqrt(0.5);
        check(approx(a->source_result("mid").gain_l, 0.5 * c),
              "linear mid: attenu 0.5");
        check(approx(a->source_result("far").gain_l, 0.0), "linear far: attenu 0");
        check(approx(a->source_result("near").gain_l, c), "linear near: attenu 1");
    }
    // Inverse / InverseSquare: min 2, max 100.
    {
        auto inv = create_spatial_audio();
        auto sq = create_spatial_audio();
        std::string err;
        AudioSpatialSpec spec = make_spec();
        check(inv->configure(spec, err), "configure inverse");
        spec.rolloff = RolloffModel::InverseSquare;
        check(sq->configure(spec, err), "configure inverse_square");
        inv->set_listener({0, 0, 0}, {1, 0, 0}, err);
        sq->set_listener({0, 0, 0}, {1, 0, 0}, err);
        inv->set_source("d10", {{10, 0, 0}, 0.0, 0.5, 0.0, true}, err);
        sq->set_source("d10", {{10, 0, 0}, 0.0, 0.5, 0.0, true}, err);
        inv->update(err);
        sq->update(err);
        const double c = std::sqrt(0.5);
        check(approx(inv->source_result("d10").gain_l, 0.2 * c),
              "inverse d=10: 0.2");
        check(approx(sq->source_result("d10").gain_l, 0.04 * c),
              "inverse_square d=10: 0.04");
    }
}

void test_panning() {
    auto a = create_spatial_audio();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    check(a->set_listener({0, 0, 0}, {1, 0, 0}, err), "listener fwd +x");
    a->set_source("right", {{0, 0, 2}, 0.0, 0.5, 0.0, true}, err);  // azimute +90°
    a->set_source("left", {{0, 0, -2}, 0.0, 0.5, 0.0, true}, err);  // azimute -90°
    a->update(err);

    const double l_plus = std::cos(3.0 * 3.14159265358979323846 / 8.0);   // pan +0.5
    const double r_plus = std::sin(3.0 * 3.14159265358979323846 / 8.0);
    const double l_minus = std::cos(3.14159265358979323846 / 8.0);        // pan -0.5
    const double r_minus = std::sin(3.14159265358979323846 / 8.0);

    const AudioSourceResult right = a->source_result("right");
    check(approx(right.gain_l, l_plus) && approx(right.gain_r, r_plus),
          "direita: L=cos(3π/8), R=sin(3π/8)");
    check(right.gain_r > right.gain_l, "direita: R > L");
    check(approx(right.gain_l * right.gain_l + right.gain_r * right.gain_r, 1.0),
          "equal-power: L²+R²=1 (direita)");

    const AudioSourceResult left = a->source_result("left");
    check(approx(left.gain_l, l_minus) && approx(left.gain_r, r_minus),
          "esquerda: L=cos(π/8), R=sin(π/8)");
    check(left.gain_l > left.gain_r, "esquerda: L > R");
}

void test_occlusion() {
    auto a = create_spatial_audio();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    check(a->set_listener({0, 0, 0}, {1, 0, 0}, err), "listener");
    a->set_source("half", {{2, 0, 0}, 0.0, 0.5, 0.5, true}, err);  // 50% ocluída
    a->set_source("full", {{2, 0, 0}, 0.0, 0.5, 1.0, true}, err);  // 100% ocluída
    a->update(err);
    const double c = std::sqrt(0.5);
    check(approx(a->source_result("half").gain_l, 0.5 * c),
          "oclusão 0.5: ganho pela metade");
    check(approx(a->source_result("full").gain_l, 0.0) &&
              approx(a->source_result("full").gain_r, 0.0),
          "oclusão 1.0: silêncio");
}

void test_reverb_zones() {
    auto a = create_spatial_audio();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    check(a->set_listener({50, 0, 0}, {1, 0, 0}, err), "listener longe");
    a->set_source("center", {{0, 0, 0}, 0.0, 0.5, 0.0, true}, err);   // wet 0.5
    a->set_source("half", {{2.5, 0, 0}, 0.0, 0.5, 0.0, true}, err);   // cov 0.5 → 0.25
    a->set_source("edge", {{5, 0, 0}, 0.0, 0.5, 0.0, true}, err);     // cov 0 → 0
    a->set_source("outside", {{20, 0, 0}, 0.0, 0.5, 0.0, true}, err); // 0
    a->update(err);
    check(approx(a->source_result("center").wet, 0.5), "centro: wet = zone.wet");
    check(approx(a->source_result("half").wet, 0.25), "meio-caminho: wet 0.25");
    check(approx(a->source_result("edge").wet, 0.0), "borda: wet 0");
    check(approx(a->source_result("outside").wet, 0.0), "fora: wet 0");
}

void test_virtualization() {
    auto a = create_spatial_audio();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    check(a->set_listener({0, 0, 0}, {1, 0, 0}, err), "listener");
    a->set_source("hi", {{2, 0, 0}, 0.0, 0.9, 0.0, true}, err);
    a->set_source("mid", {{2, 0, 0}, 0.0, 0.5, 0.0, true}, err);
    a->set_source("lo", {{2, 0, 0}, 0.0, 0.1, 0.0, true}, err);
    a->set_source("amb", {{2, 0, 0}, 0.0, 0.0, 0.0, false}, err);  // ambiente isenta
    check(a->set_max_voices(2, err), "budget 2");
    a->update(err);

    check(!a->source_result("hi").virtualized, "hi ativa (prioridade 0.9)");
    check(!a->source_result("mid").virtualized, "mid ativa (prioridade 0.5)");
    check(a->source_result("lo").virtualized, "lo virtualizada (prioridade 0.1)");
    check(!a->source_result("amb").virtualized, "ambiente isenta de virtualização");
    check(approx(a->source_result("lo").gain_l, 0.0) &&
              approx(a->source_result("lo").gain_r, 0.0),
          "virtualizada: ganhos zero");

    const auto virtualized = a->virtualized_sources();
    check(virtualized.size() == 1 && virtualized[0] == "lo",
          "virtualized_sources contém exatamente lo");

    // Desempate por id (prioridades iguais → id ASC vence).
    auto b = create_spatial_audio();
    check(b->configure(make_spec(), err), "configure b");
    b->set_listener({0, 0, 0}, {1, 0, 0}, err);
    b->set_source("a", {{2, 0, 0}, 0.0, 0.5, 0.0, true}, err);
    b->set_source("b", {{2, 0, 0}, 0.0, 0.5, 0.0, true}, err);
    b->set_max_voices(1, err);
    b->update(err);
    check(!b->source_result("a").virtualized && b->source_result("b").virtualized,
          "empate de prioridade: id ASC vence");
}

void test_state_roundtrip() {
    auto a = create_spatial_audio();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    check(a->set_listener({1, 2, 3}, {0, 0, 1}, err), "listener");
    a->set_source("s1", {{2, 0, 0}, -6.0, 0.8, 0.3, true}, err);
    a->set_source("s2", {{10, 4, -3}, 3.0, 0.2, 0.0, false}, err);
    a->set_max_voices(1, err);
    a->update(err);

    const std::string state = a->serialize_state();
    auto b = create_spatial_audio();
    check(b->configure(make_spec(), err), "configure b");
    check(b->deserialize_state(state, err) && err.empty(), "deserialize state");
    check(b->serialize_state() == state, "state round-trip bit-exact");
    b->update(err);
    check(b->source_result("s1").gain_l == a->source_result("s1").gain_l &&
              b->source_result("s2").gain_l == a->source_result("s2").gain_l,
          "resultados idênticos após restauração");

    auto c = create_spatial_audio();
    check(c->configure(make_spec(), err), "configure c");
    const std::string before = c->serialize_state();
    check(!c->deserialize_state("{\"listener\":{\"px\":0,\"py\":0,\"pz\":0,"
                                "\"fx\":1,\"fy\":0,\"fz\":0},\"max_voices\":1,"
                                "\"sources\":{\"x\":{\"px\":0,\"py\":0,\"pz\":0,"
                                "\"priority\":2}}}",
                                err) &&
              !err.empty(),
          "prioridade inválida recusa (all-or-nothing)");
    check(c->serialize_state() == before, "recusa não muta (all-or-nothing)");
}

void test_runtime_refusals() {
    auto a = create_spatial_audio();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    check(!a->set_listener({0, 0, 0}, {0, 1, 0}, err) && !err.empty(),
          "forward vertical (projeção XZ nula) recusa");
    check(!a->set_source("s", {{0, 0, 0}, 0.0, 0.5, 1.5, true}, err) &&
              !err.empty(),
          "oclusão > 1 recusa");
    check(!a->set_source("s", {{0, 0, 0}, 0.0, -0.1, 0.0, true}, err) &&
              !err.empty(),
          "prioridade negativa recusa");
    check(!a->set_max_voices(0, err) && !err.empty(), "max_voices 0 recusa");
    check(!a->remove_source("nope", err) && !err.empty(), "remove desconhecida recusa");
    check(!a->source_active("nope"), "source_active desconhecida = false");
    const AudioSourceResult r = a->source_result("nope");
    check(r.gain_l == 0.0 && r.gain_r == 0.0 && r.wet == 0.0,
          "source_result desconhecida = zeros");
}

void test_determinism() {
    auto a = create_spatial_audio();
    auto b = create_spatial_audio();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    check(b->configure(make_spec(), err), "configure b");
    for (int i = 0; i < 3; ++i) {
        a->set_listener({double(i), 0, 0}, {1, 0, 0}, err);
        b->set_listener({double(i), 0, 0}, {1, 0, 0}, err);
        a->set_source("s", {{2, 0, 2}, -3.0, 0.7, 0.2, true}, err);
        b->set_source("s", {{2, 0, 2}, -3.0, 0.7, 0.2, true}, err);
        a->update(err);
        b->update(err);
    }
    check(a->serialize_state() == b->serialize_state(),
          "determinismo: estado bit-exato");
    check(a->source_result("s").gain_l == b->source_result("s").gain_l &&
              a->source_result("s").gain_r == b->source_result("s").gain_r,
          "determinismo: resultados bit-exatos");
}

}  // namespace

int main() {
    test_spec_validate();
    test_spec_json_roundtrip();
    test_front_center_no_nan();
    test_rolloff_models();
    test_panning();
    test_occlusion();
    test_reverb_zones();
    test_virtualization();
    test_state_roundtrip();
    test_runtime_refusals();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "spatial_audio_tests: all checks passed\n";
    } else {
        std::cout << "spatial_audio_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
