// PerceptionTests — gate do contrato público de percepção/sensores (agente 4
// §3 item 34). Prova que visão (cone+FOV+alcance), audição (raio·loudness),
// proximidade, memória com TTL e ameaça/alvo são puros, determinísticos,
// all-or-nothing no JSON e bit-exact no round-trip.

#include "engine/ai/IPerception.hpp"

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

using engine::ai::Detection;
using engine::ai::IPerception;
using engine::ai::PerceptionSpec;
using engine::ai::PerceptionStimulus;
using engine::ai::Vec3;
using engine::ai::create_perception;

const Vec3 kForwardZ{0, 0, 1};

PerceptionStimulus stim(uint32_t id, const Vec3& pos, float loudness,
                        bool hostile, const std::string& kind = "") {
    PerceptionStimulus s;
    s.id = id;
    s.position = pos;
    s.loudness = loudness;
    s.hostile = hostile;
    s.kind = kind;
    return s;
}

bool has_flag(const std::vector<Detection>& dets, uint32_t id,
              bool Detection::* flag) {
    for (const auto& d : dets) {
        if (d.id == id) {
            return d.*flag;
        }
    }
    return false;
}

const Detection* find_det(const std::vector<Detection>& dets, uint32_t id) {
    for (const auto& d : dets) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

void test_configure_all_or_nothing() {
    auto p = create_perception();
    std::string err;

    PerceptionSpec bad = {};
    bad.vision_range = -1.0f;
    check(!p->configure(bad, err) && !err.empty(), "configure recusa vision_range < 0");
    bad = {};
    bad.vision_half_angle_deg = 120.0f;
    check(!p->configure(bad, err) && !err.empty(), "configure recusa half-angle > 90");
    bad = {};
    bad.memory_ttl = 0.0f;
    check(!p->configure(bad, err) && !err.empty(), "configure recusa memory_ttl <= 0");
    bad = {};
    bad.max_range = 0.0f;
    check(!p->configure(bad, err) && !err.empty(), "configure recusa max_range <= 0");
    bad = {};
    bad.vision_range = std::nanf("");
    check(!p->configure(bad, err) && !err.empty(), "configure recusa NaN");

    PerceptionSpec good = {};
    check(p->configure(good, err) && err.empty(), "configure aceita spec válida");
}

void test_spec_json_roundtrip() {
    PerceptionSpec spec;
    spec.vision_range = 20.0f;
    spec.vision_half_angle_deg = 45.0f;
    spec.hearing_range = 30.0f;
    spec.proximity_range = 1.5f;
    spec.memory_ttl = 7.0f;
    spec.max_range = 64.0f;

    const std::string json = spec.to_json();
    PerceptionSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    PerceptionSpec keep = loaded;
    check(!loaded.load_from_json("{not json", err) && !err.empty(), "spec JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "spec recusa não muta (all-or-nothing)");
    check(!loaded.load_from_json("{\"vision_range\":\"x\"}", err) && !err.empty(),
          "spec tipo errado recusa");
    check(loaded.to_json() == keep.to_json(), "spec tipo errado não muta");
}

void test_vision_cone() {
    auto p = create_perception();
    PerceptionSpec spec;
    spec.vision_range = 16.0f;
    spec.vision_half_angle_deg = 60.0f;  // cos = 0.5
    spec.hearing_range = 0.0f;           // desliga audição
    spec.proximity_range = 0.0f;
    std::string err;
    check(p->configure(spec, err), "configure");

    std::vector<PerceptionStimulus> stimuli{
        stim(1, Vec3{0, 0, 10}, 0.0f, false),   // na frente, dentro do cone
        stim(2, Vec3{0, 0, -10}, 0.0f, false),  // atrás
        stim(3, Vec3{10, 0, 5}, 0.0f, false),   // lateral fora do cone
    };
    check(p->update(Vec3{}, kForwardZ, stimuli, 0.0f, err), "update");
    auto dets = p->detections();
    check(find_det(dets, 1) != nullptr && has_flag(dets, 1, &Detection::via_vision),
          "estímulo frontal via_vision");
    check(find_det(dets, 2) == nullptr, "estímulo atrás não detectado");
    check(find_det(dets, 3) == nullptr, "estímulo fora do cone não detectado");
}

void test_hearing_radius() {
    auto p = create_perception();
    PerceptionSpec spec;
    spec.vision_range = 4.0f;     // visão curta
    spec.hearing_range = 40.0f;
    spec.proximity_range = 0.0f;
    std::string err;
    check(p->configure(spec, err), "configure");

    // dist 30 > vision, loudness 1 → hearing_range*1 = 40 ≥ 30 → audível.
    std::vector<PerceptionStimulus> stimuli{
        stim(7, Vec3{0, 0, 30}, 1.0f, false),
        stim(8, Vec3{0, 0, 30}, 0.5f, false),  // 40*0.5 = 20 < 30 → inaudível
    };
    check(p->update(Vec3{}, kForwardZ, stimuli, 0.0f, err), "update");
    auto dets = p->detections();
    check(find_det(dets, 7) != nullptr && has_flag(dets, 7, &Detection::via_hearing),
          "estímulo alto audível");
    check(find_det(dets, 8) == nullptr, "estímulo baixo inaudível");
}

void test_proximity() {
    auto p = create_perception();
    PerceptionSpec spec;
    spec.vision_range = 0.0f;
    spec.hearing_range = 0.0f;
    spec.proximity_range = 2.0f;
    std::string err;
    check(p->configure(spec, err), "configure");

    std::vector<PerceptionStimulus> stimuli{
        stim(4, Vec3{0, 0, -1}, 0.0f, false),  // atrás, silencioso, bem perto
    };
    check(p->update(Vec3{}, kForwardZ, stimuli, 0.0f, err), "update");
    auto dets = p->detections();
    check(find_det(dets, 4) != nullptr && has_flag(dets, 4, &Detection::via_proximity),
          "estímulo próximo detectado por proximidade (mesmo de costas)");
}

void test_max_range_cap() {
    auto p = create_perception();
    PerceptionSpec spec;
    spec.max_range = 128.0f;
    std::string err;
    check(p->configure(spec, err), "configure");

    std::vector<PerceptionStimulus> stimuli{
        stim(9, Vec3{0, 0, 200}, 1.0f, false),
    };
    check(p->update(Vec3{}, kForwardZ, stimuli, 0.0f, err), "update");
    check(p->detections().empty(), "estímulo além de max_range não detectado");
}

void test_nearest_threat() {
    auto p = create_perception();
    PerceptionSpec spec;
    spec.hearing_range = 1000.0f;  // todo mundo audível
    spec.proximity_range = 0.0f;
    std::string err;
    check(p->configure(spec, err), "configure");

    std::vector<PerceptionStimulus> stimuli{
        stim(1, Vec3{0, 0, 3}, 1.0f, false),   // não-hostil mais perto
        stim(5, Vec3{0, 0, 20}, 1.0f, true),
        stim(9, Vec3{0, 0, 10}, 1.0f, true),   // hostil mais próximo
    };
    check(p->update(Vec3{}, kForwardZ, stimuli, 0.0f, err), "update");
    Detection out;
    check(p->nearest_threat(out) && out.id == 9, "nearest_threat = hostil mais próximo");

    // empate → menor id
    std::vector<PerceptionStimulus> tie{
        stim(7, Vec3{0, 0, 10}, 1.0f, true),
        stim(3, Vec3{0, 0, 10}, 1.0f, true),
    };
    check(p->update(Vec3{}, kForwardZ, tie, 0.0f, err), "update");
    check(p->nearest_threat(out) && out.id == 3, "nearest_threat empata para menor id");

    // sem hostil → false
    std::vector<PerceptionStimulus> none{stim(2, Vec3{0, 0, 5}, 1.0f, false)};
    check(p->update(Vec3{}, kForwardZ, none, 0.0f, err), "update");
    check(!p->nearest_threat(out), "nearest_threat sem hostil = false");
}

void test_memory_ttl() {
    auto p = create_perception();
    PerceptionSpec spec;
    spec.vision_range = 16.0f;
    spec.hearing_range = 0.0f;
    spec.proximity_range = 0.0f;
    spec.memory_ttl = 5.0f;
    std::string err;
    check(p->configure(spec, err), "configure");

    std::vector<PerceptionStimulus> seen{stim(42, Vec3{0, 0, 5}, 0.0f, true)};
    check(p->update(Vec3{}, kForwardZ, seen, 0.0f, err), "update vê 42");
    check(p->remembered_ids().size() == 1 && p->remembered_ids()[0] == 42,
          "42 entra na memória");

    // some da vista por 2s (ttl 5) → ainda lembrado
    check(p->update(Vec3{}, kForwardZ, {}, 2.0f, err), "update vazio 2s");
    check(p->remembered_ids().size() == 1, "42 lembrado após 2s");

    // +4s (idade 6 > 5) → esquecido
    check(p->update(Vec3{}, kForwardZ, {}, 4.0f, err), "update vazio 4s");
    check(p->remembered_ids().empty(), "42 esquecido após ttl");
}

void test_state_roundtrip() {
    auto p = create_perception();
    PerceptionSpec spec;
    spec.vision_range = 16.0f;
    spec.hearing_range = 0.0f;
    spec.proximity_range = 0.0f;
    spec.memory_ttl = 10.0f;
    std::string err;
    check(p->configure(spec, err), "configure");

    std::vector<PerceptionStimulus> stimuli{
        stim(5, Vec3{0, 0, 3}, 0.0f, true),
        stim(9, Vec3{0, 0, 6}, 0.0f, false),
    };
    check(p->update(Vec3{}, kForwardZ, stimuli, 1.0f, err), "update");

    const std::string state = p->serialize_state();
    auto q = create_perception();
    check(q->configure(spec, err), "configure q");
    check(q->deserialize_state(state, err) && err.empty(), "deserialize state");
    check(q->serialize_state() == state, "state round-trip bit-exact");
    check(q->remembered_ids() == p->remembered_ids(), "remembered_ids idênticos");

    auto r = create_perception();
    check(!r->deserialize_state("{bad", err) && !err.empty(), "state inválido recusa");
    check(!r->deserialize_state("{\"memory\":[{\"id\":-1,\"age\":0}]}", err) &&
              !err.empty(),
          "state id negativo recusa");
}

void test_determinism() {
    PerceptionSpec spec;
    spec.vision_range = 20.0f;
    spec.hearing_range = 40.0f;
    spec.proximity_range = 2.0f;
    spec.memory_ttl = 8.0f;

    std::vector<PerceptionStimulus> stimuli{
        stim(1, Vec3{1, 2, 3}, 0.9f, true, "enemy"),
        stim(2, Vec3{-4, 0, 8}, 0.4f, false, "item"),
    };

    auto a = create_perception();
    auto b = create_perception();
    std::string err;
    check(a->configure(spec, err), "configure a");
    check(b->configure(spec, err), "configure b");
    check(a->update(Vec3{0, 1, 0}, Vec3{0.3f, 0, 1}, stimuli, 0.25f, err), "update a");
    check(b->update(Vec3{0, 1, 0}, Vec3{0.3f, 0, 1}, stimuli, 0.25f, err), "update b");

    const auto da = a->detections();
    const auto db = b->detections();
    check(da.size() == db.size(), "determinismo: mesma contagem de detecções");
    bool same = da.size() == db.size();
    for (std::size_t i = 0; same && i < da.size(); ++i) {
        same = da[i].id == db[i].id && da[i].position.approx(db[i].position, 0.0f) &&
               da[i].via_vision == db[i].via_vision &&
               da[i].via_hearing == db[i].via_hearing &&
               da[i].via_proximity == db[i].via_proximity &&
               da[i].distance == db[i].distance;
    }
    check(same, "determinismo: detecções bit-exatas");
    check(a->serialize_state() == b->serialize_state(), "determinismo: estado bit-exato");
}

}  // namespace

// A2-114 (Agente 5): the sensors propagate real faction + damage signals into
// the detections, not just hostile + kind — the game feeds faction/damage on
// each stimulus and nearest_threat/nearest reading expose them.
void test_faction_and_damage_sensed() {
    auto p = create_perception();
    PerceptionSpec spec;
    spec.vision_range = 20.0f;  // wide cone: everything in front is seen
    std::string err;
    check(p->configure(spec, err), "configure");

    PerceptionStimulus s;
    s.id = 7;
    s.position = Vec3{ 0, 0, 5 };
    s.loudness = 1.0f;
    s.hostile = true;
    s.kind = "mob";
    s.faction = "bandit";
    s.damage = 42.0f;
    std::string uerr;
    check(p->update(Vec3{ 0, 0, 0 }, kForwardZ, { s }, 0.016f, uerr),
          "update with faction+damage stimulus");

    const auto dets = p->detections();
    check(dets.size() == 1, "damage/faction stimulus detected");
    const Detection* d = find_det(dets, 7);
    check(d != nullptr && d->faction == "bandit",
          "faction propagates into detection");
    check(d != nullptr && d->damage == 42.0f,
          "damage propagates into detection");
    check(d != nullptr && d->hostile, "hostile propagates into detection");

    // nearest_threat also exposes the sensed faction + damage (what the game
    // reads for the percept title segment).
    Detection threat;
    check(p->nearest_threat(threat), "nearest_threat present");
    check(threat.faction == "bandit" && threat.damage == 42.0f,
          "nearest_threat carries faction+damage");
    std::cout << "[percept] sensed faction=" << threat.faction
              << " damage=" << threat.damage << "\n";
}

int main() {
    test_configure_all_or_nothing();
    test_spec_json_roundtrip();
    test_vision_cone();
    test_hearing_radius();
    test_proximity();
    test_max_range_cap();
    test_nearest_threat();
    test_memory_ttl();
    test_state_roundtrip();
    test_determinism();
    test_faction_and_damage_sensed();

    if (g_failures == 0) {
        std::cout << "perception_tests: all checks passed\n";
    } else {
        std::cout << "perception_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
