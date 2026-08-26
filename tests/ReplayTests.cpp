// ReplayTests — gate do contrato IReplay (§5 item 60, replay determinístico).
// Prova: gravação exige ticks estritamente crescentes (all-or-nothing),
// reprodução bit-exact na ordem gravada, seek por tick, trim do fim,
// limite de frames, JSON round-trip bit-exact e rejeição de documento
// inválido com estado intacto.

#include "engine/gameplay/IReplay.hpp"

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

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    for (const int value : values) out.push_back(static_cast<std::uint8_t>(value));
    return out;
}

void test_recording_and_playback() {
    auto replay = engine::gameplay::create_replay();
    std::string error;

    check(replay->frame_count() == 0, "vazio no início");
    check(replay->first_tick() == 0 && replay->last_tick() == 0, "ticks vazios = 0");

    check(replay->record_tick(10, 100, bytes({1, 2, 3}), error), "grava tick 10");
    check(replay->record_tick(20, 200, bytes({4, 5}), error), "grava tick 20");
    check(replay->record_tick(30, 300, bytes({6}), error), "grava tick 30");
    check(replay->frame_count() == 3, "3 frames gravados");
    check(replay->first_tick() == 10 && replay->last_tick() == 30, "first/last tick");

    // Fora de ordem → rejeitado e nada adicionado (all-or-nothing).
    check(!replay->record_tick(20, 999, bytes({9}), error), "tick duplicado rejeitado");
    check(!replay->record_tick(5, 999, bytes({9}), error), "tick decrescente rejeitado");
    check(replay->frame_count() == 3, "nenhum frame adicionado após rejeição");

    // Reprodução bit-exact na ordem gravada.
    check(replay->begin_replay(error), "begin_replay");
    engine::gameplay::ReplayFrame frame;
    check(replay->next_frame(frame) && frame.tick == 10 && frame.seed == 100 &&
              frame.inputs == bytes({1, 2, 3}),
          "frame 1 bit-exact");
    check(replay->next_frame(frame) && frame.tick == 20 && frame.seed == 200 &&
              frame.inputs == bytes({4, 5}),
          "frame 2 bit-exact");
    check(replay->next_frame(frame) && frame.tick == 30 && frame.seed == 300 &&
              frame.inputs == bytes({6}),
          "frame 3 bit-exact");
    check(!replay->next_frame(frame), "fim da reprodução");

    // Seek: posiciona no primeiro frame com tick >= alvo.
    check(replay->seek_tick(20), "seek tick 20");
    check(replay->next_frame(frame) && frame.tick == 20, "seek entrega frame 20");
    check(replay->seek_tick(25), "seek tick 25 (entre frames)");
    check(replay->next_frame(frame) && frame.tick == 30, "seek 25 entrega frame 30");
    check(!replay->seek_tick(99), "seek além do fim falha");
}

void test_truncate_and_limit() {
    auto replay = engine::gameplay::create_replay(2);  // max 2 frames
    std::string error;

    check(replay->record_tick(1, 11, bytes({1}), error), "grava 1");
    check(replay->record_tick(2, 22, bytes({2}), error), "grava 2");
    check(!replay->record_tick(3, 33, bytes({3}), error), "limite de frames rejeita o 3º");
    check(replay->frame_count() == 2, "limite respeitado");
    check(replay->last_tick() == 2, "último tick inalterado");

    check(replay->truncate_after(1, error), "trim após tick 1");
    check(replay->frame_count() == 1, "1 frame após trim");
    check(replay->last_tick() == 1, "último tick = 1 após trim");
    check(replay->truncate_after(0, error), "trim após tick 0 (esvazia)");
    check(replay->frame_count() == 0, "vazio após trim total");
}

void test_json_round_trip() {
    auto replay = engine::gameplay::create_replay();
    std::string error;

    replay->record_tick(10, 100, bytes({1, 2, 3}), error);
    replay->record_tick(20, 200, bytes({}), error);
    const std::string json = replay->serialize_state();
    check(!json.empty(), "serialize gera documento");

    auto loaded = engine::gameplay::create_replay();
    check(loaded->load_from_json(json, error), "load do documento");
    check(loaded->frame_count() == 2, "2 frames após load");
    check(loaded->serialize_state() == json, "round-trip bit-exact");

    // Rejeições: documento inválido → estado intacto.
    const std::string valid = loaded->serialize_state();
    check(!loaded->load_from_json("{\"frames\":[{\"tick\":1,\"seed\":1,\"inputs\":[]},"
                                  "{\"tick\":1,\"seed\":2,\"inputs\":[]}]}", error),
          "tick duplicado no documento rejeitado");
    check(loaded->serialize_state() == valid, "estado intacto após rejeição");
    check(!loaded->load_from_json("{\"frames\":[{\"tick\":1,\"seed\":1,\"inputs\":[300]}]}", error),
          "byte fora de faixa rejeitado");
    check(loaded->serialize_state() == valid, "estado intacto após byte inválido");
    check(!loaded->load_from_json("{\"frames\":[{\"tick\":1,\"seed\":1}]}", error),
          "frame incompleto rejeitado");
    check(!loaded->load_from_json("{\"nada\":1}", error), "campo frames ausente rejeitado");
    check(!loaded->load_from_json("não-json", error), "não-json rejeitado");
    check(loaded->serialize_state() == valid, "estado intacto após rejeições");
}

}  // namespace

int main() {
    test_recording_and_playback();
    test_truncate_and_limit();
    test_json_round_trip();

    if (failures == 0) {
        std::printf("replay_tests: all checks passed\n");
        return 0;
    }
    std::printf("replay_tests: %d failure(s)\n", failures);
    return 1;
}
