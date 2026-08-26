// AiLodTests — gate do contrato público de LOD de IA por entidade (agente 4
// §3 item 5). Prova que a classificação por distância, os intervalos de
// update, os budgets com rebaixamento determinístico e o JSON são
// all-or-nothing/bit-exact/determinísticos como documentado.

#include "engine/ai/IAiLod.hpp"

#include <cstdint>
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

using engine::ai::AiLodAllocation;
using engine::ai::AiLodEntry;
using engine::ai::AiLodSpec;
using engine::ai::AiLodTier;
using engine::ai::create_ai_lod;

AiLodSpec make_spec() {
    AiLodSpec spec;
    spec.full_radius = 16.0;
    spec.reduced_radius = 64.0;
    spec.aggregate_radius = 256.0;
    spec.reduced_interval = 4.0;
    spec.aggregate_interval = 16.0;
    return spec;
}

void test_validate_all_or_nothing() {
    AiLodSpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    AiLodSpec bad = s;
    bad.full_radius = -1.0;
    check(!bad.validate(err) && !err.empty(), "full_radius negativo recusa");

    bad = s;
    bad.reduced_radius = s.full_radius - 1.0;
    check(!bad.validate(err) && !err.empty(), "reduced < full recusa");

    bad = s;
    bad.reduced_interval = 0.5;
    check(!bad.validate(err) && !err.empty(), "intervalo < 1 recusa");

    bad = s;
    bad.max_full = -1;
    check(!bad.validate(err) && !err.empty(), "budget negativo recusa");
}

void test_spec_json_roundtrip() {
    const AiLodSpec spec = make_spec();
    const std::string json = spec.to_json();
    AiLodSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    AiLodSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
    check(!loaded.load_from_json("{\"version\":2}", err) && !err.empty(),
          "versão desconhecida recusa");
}

void test_tier_for() {
    auto l = create_ai_lod();
    std::string err;
    check(l->configure(make_spec(), err), "configure");
    check(l->tier_for(0.0) == AiLodTier::Full, "dist 0 → Full");
    check(l->tier_for(16.0) == AiLodTier::Full, "dist == full_radius → Full");
    check(l->tier_for(16.0001) == AiLodTier::Reduced, "dist > full → Reduced");
    check(l->tier_for(64.0) == AiLodTier::Reduced, "dist == reduced → Reduced");
    check(l->tier_for(64.0001) == AiLodTier::Aggregate, "dist > reduced → Aggregate");
    check(l->tier_for(256.0) == AiLodTier::Aggregate, "dist == aggregate → Aggregate");
    check(l->tier_for(256.0001) == AiLodTier::Dormant, "dist > aggregate → Dormant");
}

void test_should_update() {
    auto l = create_ai_lod();
    std::string err;
    check(l->configure(make_spec(), err), "configure");
    check(l->should_update(AiLodTier::Full, 0) &&
              l->should_update(AiLodTier::Full, 7),
          "Full atualiza sempre");
    check(!l->should_update(AiLodTier::Dormant, 0) &&
              !l->should_update(AiLodTier::Dormant, 99),
          "Dormant nunca atualiza");
    // Reduced interval 4: ticks 0,4,8 atualizam; 1,2,3 não.
    check(l->should_update(AiLodTier::Reduced, 0) &&
              l->should_update(AiLodTier::Reduced, 4) &&
              !l->should_update(AiLodTier::Reduced, 1) &&
              !l->should_update(AiLodTier::Reduced, 3),
          "Reduced por módulo 4");
    // Aggregate interval 16: tick 0,16 atualizam; 4 não.
    check(l->should_update(AiLodTier::Aggregate, 0) &&
              l->should_update(AiLodTier::Aggregate, 16) &&
              !l->should_update(AiLodTier::Aggregate, 4),
          "Aggregate por módulo 16");
}

void test_allocate_budgets_demote_furthest() {
    AiLodSpec spec = make_spec();
    spec.max_full = 2;   // só 2 ficam em Full
    spec.max_reduced = 1;
    auto l = create_ai_lod();
    std::string err;
    check(l->configure(spec, err), "configure");

    // 3 em Full (5, 10, 15); budget 2 → o mais distante (15) vai para Reduced.
    // 2 em Reduced (20, 30); budget 1 → o mais distante (30) vai para Aggregate.
    const std::vector<AiLodEntry> entries{
        {1, 5.0}, {2, 10.0}, {3, 15.0}, {4, 20.0}, {5, 30.0},
    };
    const auto allocs = l->allocate(0, entries);

    auto tier_of = [&](std::uint64_t id) -> AiLodTier {
        for (const auto& a : allocs) {
            if (a.id == id) return a.tier;
        }
        return AiLodTier::Dormant;  // não deveria acontecer
    };
    check(tier_of(1) == AiLodTier::Full && tier_of(2) == AiLodTier::Full &&
              tier_of(3) == AiLodTier::Reduced,
          "excesso do Full → o mais distante rebaixado para Reduced");
    check(tier_of(4) == AiLodTier::Aggregate && tier_of(5) == AiLodTier::Aggregate,
          "excesso do Reduced (budget 1) → rebaixados para Aggregate");
    // update flags: tick 0 → Full e Reduced atualizam (0 % 4 == 0); Aggregate também.
    for (const auto& a : allocs) {
        check(a.update, "tick 0 atualiza todo mundo (módulos zeram)");
    }
}

void test_allocate_tie_breaks_by_id() {
    AiLodSpec spec = make_spec();
    spec.max_full = 1;
    auto l = create_ai_lod();
    std::string err;
    check(l->configure(spec, err), "configure");

    // empate em distância (10.0): budget 1 → o de MAIOR id é rebaixado.
    const std::vector<AiLodEntry> entries{{7, 10.0}, {3, 10.0}};
    const auto allocs = l->allocate(0, entries);
    auto tier_of = [&](std::uint64_t id) -> AiLodTier {
        for (const auto& a : allocs) {
            if (a.id == id) return a.tier;
        }
        return AiLodTier::Dormant;
    };
    check(tier_of(3) == AiLodTier::Full && tier_of(7) == AiLodTier::Reduced,
          "empate de distância → maior id rebaixado primeiro (determinístico)");
}

void test_allocate_interval_flags() {
    auto l = create_ai_lod();
    std::string err;
    check(l->configure(make_spec(), err), "configure");

    const std::vector<AiLodEntry> entries{
        {1, 5.0},    // Full
        {2, 30.0},   // Reduced
        {3, 100.0},  // Aggregate
        {4, 500.0},  // Dormant
    };
    const auto allocs = l->allocate(5, entries);
    check(allocs.size() == 4, "alocações para todos");
    bool saw_full = false, saw_reduced = false, saw_agg = false, saw_dorm = false;
    for (const auto& a : allocs) {
        if (a.id == 1) { saw_full = true; check(a.update, "Full atualiza no tick 5"); }
        if (a.id == 2) { saw_reduced = true; check(!a.update, "Reduced não atualiza no tick 5 (5 % 4 != 0)"); }
        if (a.id == 3) { saw_agg = true; check(!a.update, "Aggregate não atualiza no tick 5"); }
        if (a.id == 4) { saw_dorm = true; check(!a.update, "Dormant nunca atualiza"); }
    }
    check(saw_full && saw_reduced && saw_agg && saw_dorm, "todas as alocações presentes");
}

void test_determinism() {
    auto a = create_ai_lod();
    auto b = create_ai_lod();
    std::string err;
    AiLodSpec spec = make_spec();
    spec.max_full = 2;
    spec.max_reduced = 2;
    check(a->configure(spec, err), "configure a");
    check(b->configure(spec, err), "configure b");

    std::vector<AiLodEntry> entries;
    for (std::uint64_t i = 0; i < 20; ++i) {
        entries.push_back(AiLodEntry{i, static_cast<double>(i * 3)});
    }
    for (std::uint64_t tick = 0; tick < 32; ++tick) {
        const auto aa = a->allocate(tick, entries);
        const auto bb = b->allocate(tick, entries);
        bool same = aa.size() == bb.size();
        for (std::size_t i = 0; same && i < aa.size(); ++i) {
            same = aa[i].id == bb[i].id && aa[i].tier == bb[i].tier &&
                   aa[i].update == bb[i].update;
        }
        check(same, "determinismo: alocações bit-exatas cross-instance");
    }
}

}  // namespace

int main() {
    test_validate_all_or_nothing();
    test_spec_json_roundtrip();
    test_tier_for();
    test_should_update();
    test_allocate_budgets_demote_furthest();
    test_allocate_tie_breaks_by_id();
    test_allocate_interval_flags();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "ai_lod_tests: all checks passed\n";
    } else {
        std::cout << "ai_lod_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
