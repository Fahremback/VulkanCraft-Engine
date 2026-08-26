// TimelinePolicyTests — gate do contrato ITimelinePolicy (§6 item 68,
// política de timeline CORE): prova prune (orçamento de retenção
// determinístico — mantém os maxStates lexicograficamente primeiros),
// compact (dedup de snapshots idênticos por mundo+path), recusas de config.

#include "engine/world/ITimelinePolicy.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

bool strings_equal(const std::vector<std::string>& actual,
                   const std::vector<std::string>& expected, const char* what) {
    bool ok = actual.size() == expected.size();
    if (ok) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (actual[i] != expected[i]) ok = false;
        }
    }
    if (!ok) {
        std::printf("    actual:   ");
        for (const auto& s : actual) std::printf("[%s] ", s.c_str());
        std::printf("\n    expected: ");
        for (const auto& s : expected) std::printf("[%s] ", s.c_str());
        std::printf("\n");
    }
    check(ok, what);
    return ok;
}

engine::world::TimelinePolicyState state(const std::string& name,
                                         const std::string& world,
                                         const std::string& path) {
    engine::world::TimelinePolicyState s;
    s.name = name; s.worldName = world; s.path = path;
    return s;
}

void test_prune() {
    auto policy = engine::world::create_timeline_policy();
    std::string error;
    engine::world::TimelinePolicyConfig config;
    config.maxStates = 3;
    check(policy->configure(config, error), "configure maxStates 3");
    check(policy->max_states() == 3, "max_states");

    // 5 estados, orçamento 3 → remove os 2 lexicograficamente maiores.
    std::vector<engine::world::TimelinePolicyState> states;
    states.push_back(state("delta", "w1", "/s/delta.vcw"));
    states.push_back(state("alpha", "w1", "/s/alpha.vcw"));
    states.push_back(state("charlie", "w2", "/s/charlie.vcw"));
    states.push_back(state("bravo", "w1", "/s/bravo.vcw"));
    states.push_back(state("echo", "w3", "/s/echo.vcw"));
    const std::vector<std::string> toRemove = policy->prune(states);
    strings_equal(toRemove, { "delta", "echo" },
                  "prune remove delta e echo (mantém alpha/bravo/charlie)");

    // Dentro do orçamento → nada a remover.
    std::vector<engine::world::TimelinePolicyState> small;
    small.push_back(state("a", "w", "p"));
    small.push_back(state("b", "w", "p"));
    check(policy->prune(small).empty(), "abaixo do orçamento → vazio");
}

void test_compact() {
    auto policy = engine::world::create_timeline_policy();
    std::string error;
    engine::world::TimelinePolicyConfig config;
    config.compactionEnabled = true;
    check(policy->configure(config, error), "configure com compactação");

    // Duplicatas exatas (mesmo mundo + mesmo path) → manter o menor nome.
    std::vector<engine::world::TimelinePolicyState> states;
    states.push_back(state("zcopy", "w1", "/s/snap.vcw"));
    states.push_back(state("acopy", "w1", "/s/snap.vcw"));   // dup de zcopy
    states.push_back(state("solo", "w1", "/s/other.vcw"));   // único
    states.push_back(state("bdup", "w2", "/s/x.vcw"));
    states.push_back(state("cdup", "w2", "/s/x.vcw"));       // dup de bdup
    const std::vector<std::string> toCompact = policy->compact(states);
    strings_equal(toCompact, { "cdup", "zcopy" },
                  "compact remove duplicatas (mantém acopy e bdup)");

    // Sem duplicatas → vazio.
    std::vector<engine::world::TimelinePolicyState> unique;
    unique.push_back(state("a", "w1", "p1"));
    unique.push_back(state("b", "w1", "p2"));
    check(policy->compact(unique).empty(), "sem duplicatas → vazio");

    // Compactação desabilitada → vazio mesmo com duplicatas.
    engine::world::TimelinePolicyConfig off;
    off.compactionEnabled = false;
    check(policy->configure(off, error), "config com compactação OFF");
    check(policy->compact(states).empty(), "compact OFF → vazio");
}

void test_config_refusals() {
    auto policy = engine::world::create_timeline_policy();
    std::string error;
    engine::world::TimelinePolicyConfig config;
    config.maxStates = 0;
    check(!policy->configure(config, error), "maxStates 0 recusa");
    config.maxStates = 4;
    check(policy->configure(config, error), "config válida aceita");
}

}  // namespace

int main() {
    test_prune();
    test_compact();
    test_config_refusals();

    if (failures == 0) {
        std::printf("timeline_policy_tests: all checks passed\n");
        return 0;
    }
    std::printf("timeline_policy_tests: %d failure(s)\n", failures);
    return 1;
}
