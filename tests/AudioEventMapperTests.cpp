// AudioEventMapperTests — gate do contrato IAudioEventMapper (§7 item 76,
// eventos→áudio CORE): prova mapeamento eventKind→trigger, ordem
// determinística, recusas all-or-nothing (eventKind vazio/duplicado, soundId
// vazio, volume fora de [0,1], pitch <= 0).

#include "engine/audio/IAudioEventMapper.hpp"

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

void test_mapping() {
    auto mapper = engine::audio::create_audio_event_mapper();
    std::string error;
    std::vector<engine::audio::AudioTrigger> triggers;
    triggers.push_back({ "explosion", "boom_01", 0.9f, 1.0f });
    triggers.push_back({ "footstep_stone", "step_stone", 0.4f, 1.2f });
    triggers.push_back({ "portal", "portal_whoosh", 0.7f, 0.8f });
    check(mapper->configure(triggers, error), "configure 3 triggers");
    check(mapper->count() == 3, "3 triggers");

    const engine::audio::AudioTrigger* boom = mapper->trigger_for("explosion");
    check(boom != nullptr && boom->soundId == "boom_01" && boom->volume == 0.9f,
          "trigger_for explosion");
    check(mapper->trigger_for("ghost") == nullptr, "trigger_for desconhecido → nullptr");

    const std::vector<engine::audio::AudioTrigger> all = mapper->triggers();
    check(all.size() == 3 && all[0].eventKind == "explosion" &&
              all[2].eventKind == "portal",
          "triggers em ordem crescente de eventKind");
}

void test_refusals() {
    auto mapper = engine::audio::create_audio_event_mapper();
    std::string error;
    check(mapper->configure({ { "a", "snd_a", 1.0f, 1.0f } }, error), "baseline");
    const std::size_t intact = mapper->count();

    std::vector<engine::audio::AudioTrigger> bad;
    bad.push_back({ "", "s", 1.0f, 1.0f });
    check(!mapper->configure(bad, error), "eventKind vazio recusa");
    bad.clear();
    bad.push_back({ "a", "s", 1.0f, 1.0f });
    bad.push_back({ "a", "s2", 1.0f, 1.0f });
    check(!mapper->configure(bad, error), "eventKind duplicado recusa");
    bad.clear();
    bad.push_back({ "b", "", 1.0f, 1.0f });
    check(!mapper->configure(bad, error), "soundId vazio recusa");
    bad.clear();
    bad.push_back({ "c", "s", 1.5f, 1.0f });
    check(!mapper->configure(bad, error), "volume > 1 recusa");
    bad.clear();
    bad.push_back({ "d", "s", 0.5f, 0.0f });
    check(!mapper->configure(bad, error), "pitch 0 recusa");
    check(mapper->count() == intact, "estado intacto após recusas");
}

}  // namespace

int main() {
    test_mapping();
    test_refusals();

    if (failures == 0) {
        std::printf("audio_event_mapper_tests: all checks passed\n");
        return 0;
    }
    std::printf("audio_event_mapper_tests: %d failure(s)\n", failures);
    return 1;
}
