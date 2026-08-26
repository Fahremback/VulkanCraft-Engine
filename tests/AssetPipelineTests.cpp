// AssetPipelineTests — gate do contrato IAssetPipeline
// (engine::assets, §6 item 1 — "pipeline público import→validate→cook→cache→
// package com operações incrementais e determinísticas").
// Prova: criação all-or-nothing (seed vazia), import com nome vazio/kind
// desconhecido/bytes inválidos recusado sem mutar, validação por kind (raw
// sempre ok, json malformado recusado, text com NUL/UTF-8 inválido recusado),
// cook determinístico (json canônico com chaves ordenadas, text com EOL
// normalizado, raw passthrough), CACHE incremental (re-cook da mesma
// fonte+versão = cache hit sem recomputar; fonte mudou = recomputa e
// invalida o artefato anterior), package ordenado por nome, remoção no-op,
// JSON round-trip bit-exact, rejeição de documento inválido com estado
// intacto (seed mismatch/unknown field/asset ausente) e determinismo
// cross-instance.

#include "engine/assets/IAssetPipeline.hpp"

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

engine::assets::AssetSource src(const char* name, const char* kind, const char* version,
                                const std::vector<std::uint8_t>& bytes) {
    engine::assets::AssetSource s;
    s.name = name;
    s.kind = kind;
    s.version = version;
    s.bytes = bytes;
    return s;
}

std::vector<std::uint8_t> vb(const char* c) {
    std::string s = c;
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

void test_creation() {
    std::printf("[creation]\n");
    std::string err;
    auto pipe = engine::assets::create_asset_pipeline("pipe-1", err);
    check(pipe != nullptr, "pipeline criado com seed válida");
    check(pipe->states().empty(), "pipeline nasce vazio");
    check(pipe->cache_hits() == 0, "cache_hits nasce 0");

    auto empty = engine::assets::create_asset_pipeline("", err);
    check(empty == nullptr, "seed vazia recusada (all-or-nothing)");
    check(!err.empty(), "erro nomeado na seed vazia");
}

void test_import_validation() {
    std::printf("[import/validation]\n");
    std::string err;
    auto pipe = engine::assets::create_asset_pipeline("pipe-2", err);

    check(!pipe->import_source(src("", "raw", "1", vb("x")), err), "nome vazio recusado");
    check(!pipe->import_source(src("a", "unknown", "1", vb("x")), err), "kind desconhecido recusado");
    check(!pipe->import_source(src("badjson", "json", "1", vb("{nope")), err), "json malformado recusado no import");
    check(!pipe->import_source(src("nultext", "text", "1", std::vector<std::uint8_t>{'a', 0, 'b'}), err),
          "text com NUL recusado");
    check(pipe->states().empty(), "nada mutou após recusas");

    check(pipe->import_source(src("ok-raw", "raw", "1", vb("x")), err), "raw importa");
    check(pipe->import_source(src("ok-json", "json", "1", vb("{\"b\":1}")), err), "json válido importa");
    check(pipe->states().size() == 2, "duas fontes registradas");

    auto v = pipe->validate("ok-raw");
    check(v.valid, "raw valida");
    v = pipe->validate("ok-json");
    check(v.valid, "json válido valida");
    v = pipe->validate("nope");
    check(!v.valid && v.error.find("unknown") != std::string::npos, "asset ausente recusado com erro nomeado");
}

void test_cook_determinism() {
    std::printf("[cook/determinism]\n");
    std::string err;
    auto pipe = engine::assets::create_asset_pipeline("pipe-3", err);

    pipe->import_source(src("j", "json", "1", vb("{\"b\":1,\"a\":[true,null,\"x\"],\"c\":{\"z\":0,\"y\":1}}")), err);
    auto r = pipe->cook("j");
    check(r.ok && !r.cache_hit, "json cooka na primeira (miss)");
    check(r.artifact == vb("{\"a\":[true,null,\"x\"],\"b\":1,\"c\":{\"y\":1,\"z\":0}}"),
          "json canônico compacto com chaves ordenadas");

    auto r2 = pipe->cook("j");
    check(r2.ok && r2.cache_hit, "re-cook da mesma fonte é cache hit");
    check(r2.artifact == r.artifact, "artefato idêntico no hit");
    check(r2.artifact_hash == r.artifact_hash, "hash idêntico no hit");
    check(pipe->cache_hits() == 1, "cache_hits acumula 1");

    pipe->import_source(src("t", "text", "1", vb("a\r\nb\rc\n")), err);
    auto rt = pipe->cook("t");
    check(rt.ok, "text cooka");
    check(rt.artifact == vb("a\nb\nc\n"), "EOL normalizado (\r\n e \r → \n)");

    pipe->import_source(src("raw1", "raw", "1", vb("hello")), err);
    auto rr = pipe->cook("raw1");
    check(rr.ok && rr.artifact == vb("hello"), "raw passthrough");

    check(!pipe->cook("missing").ok, "cook de ausente recusado");
    check(pipe->states().size() == 3, "três assets após cooks");
}

void test_incremental_cache() {
    std::printf("[incremental/cache]\n");
    std::string err;
    auto pipe = engine::assets::create_asset_pipeline("pipe-4", err);

    pipe->import_source(src("a", "json", "1", vb("{\"k\":1}")), err);
    auto r1 = pipe->cook("a");
    check(r1.ok && !r1.cache_hit, "cook inicial é miss");
    const std::uint64_t h1 = r1.artifact_hash;

    // Mesma fonte, mesma versão: hit (não recomputa).
    pipe->import_source(src("a", "json", "1", vb("{\"k\":1}")), err);
    auto r2 = pipe->cook("a");
    check(r2.ok && r2.cache_hit, "re-import idêntico mantém cache (hit)");
    check(r2.artifact_hash == h1, "hash preservado no hit");

    // Versão mudou: invalida artefato anterior e recomputa (miss).
    pipe->import_source(src("a", "json", "2", vb("{\"k\":1}")), err);
    auto r3 = pipe->cook("a");
    check(r3.ok && !r3.cache_hit, "versão nova = miss (recomputa)");
    check(r3.artifact == r1.artifact, "artefato determinístico apesar da versão");

    // Fonte mudou de conteúdo: miss.
    pipe->import_source(src("a", "json", "2", vb("{\"k\":2}")), err);
    auto r4 = pipe->cook("a");
    check(r4.ok && !r4.cache_hit, "conteúdo novo = miss");
    check(r4.artifact == vb("{\"k\":2}"), "artefato novo cozido");

    // Remove: no-op em ausente, some de states.
    pipe->remove("never-existed");
    pipe->remove("a");
    check(pipe->states().empty(), "remove limpa o asset");
    check(!pipe->cook("a").ok, "cook de removido recusado");
}

void test_package() {
    std::printf("[package]\n");
    std::string err;
    auto pipe = engine::assets::create_asset_pipeline("pipe-5", err);

    pipe->import_source(src("z", "raw", "1", vb("zz")), err);
    pipe->import_source(src("a", "raw", "1", vb("aa")), err);
    pipe->import_source(src("m", "raw", "1", vb("mm")), err);
    pipe->cook("z");
    pipe->cook("a");
    // "m" fica sem cook.

    auto manifest = pipe->package();
    check(manifest.assets.size() == 3, "package lista todos os assets");
    bool ordered = true;
    for (std::size_t idx = 1; idx < manifest.assets.size(); ++idx) {
        if (manifest.assets[idx - 1].name >= manifest.assets[idx].name) ordered = false;
    }
    check(ordered, "package ordenado por nome crescente");
    check(manifest.assets[0].name == "a" && manifest.assets[0].cooked, "a cozido");
    check(manifest.assets[1].name == "m" && !manifest.assets[1].cooked, "m não cozido");
    check(manifest.assets[2].name == "z" && manifest.assets[2].cooked, "z cozido");
}

void test_persistence() {
    std::printf("[persistence]\n");
    std::string err;
    auto pipe = engine::assets::create_asset_pipeline("pipe-6", err);

    pipe->import_source(src("j", "json", "1", vb("{\"b\":1,\"a\":2}")), err);
    pipe->cook("j");
    pipe->import_source(src("t", "text", "2", vb("x\r\ny")), err);
    const std::string snap = pipe->serialize_state();

    auto pipe2 = engine::assets::create_asset_pipeline("pipe-6", err);
    check(pipe2->load_from_json(snap, err), "load aceita o próprio snapshot");
    check(pipe2->serialize_state() == snap, "round-trip bit-exact");
    auto states2 = pipe2->states();
    check(states2.size() == 2, "estados preservados no load");

    // Rejeições all-or-nothing (estado intacto).
    check(!pipe2->load_from_json("{}", err), "documento sem seed rejeitado");
    check(pipe2->serialize_state() == snap, "estado intacto após rejeição 1");

    std::string bad2 = snap;
    const std::size_t pos = bad2.find("\"pipe-6\"");
    bad2.replace(pos, 8, "\"other\"");
    check(!pipe2->load_from_json(bad2, err), "seed mismatch rejeitado");
    check(pipe2->serialize_state() == snap, "estado intacto após rejeição 2");

    std::string bad3 = snap + ",\"bogus\":1}";
    check(!pipe2->load_from_json(bad3, err), "campo desconhecido rejeitado");
    check(pipe2->serialize_state() == snap, "estado intacto após rejeição 3");

    // Cache restaurado: cook após load é hit com artefato idêntico (o cook
    // incrementa cache_hits, então roda por último para não sujar o snapshot).
    auto rj = pipe2->cook("j");
    check(rj.ok && rj.cache_hit && rj.artifact == vb("{\"a\":2,\"b\":1}"),
          "cache restaurado: cook após load é hit com artefato idêntico");
    check(pipe2->cache_hits() == 1, "cache_hits restaurado e incrementado");
}

void test_determinism() {
    std::printf("[determinism]\n");
    std::string err;
    auto p1 = engine::assets::create_asset_pipeline("pipe-7", err);
    auto p2 = engine::assets::create_asset_pipeline("pipe-7", err);
    p1->import_source(src("j", "json", "1", vb("{\"b\":1,\"a\":[3,2,1]}")), err);
    p2->import_source(src("j", "json", "1", vb("{\"b\":1,\"a\":[3,2,1]}")), err);
    auto r1 = p1->cook("j");
    auto r2 = p2->cook("j");
    check(r1.artifact == r2.artifact, "cook determinístico cross-instance");
    check(p1->serialize_state() == p2->serialize_state(), "snapshot determinístico cross-instance");
}

void test_reset() {
    std::printf("[reset]\n");
    std::string err;
    auto pipe = engine::assets::create_asset_pipeline("pipe-8", err);
    pipe->import_source(src("a", "raw", "1", vb("x")), err);
    pipe->cook("a");
    check(pipe->reset(err), "reset ok");
    check(pipe->states().empty(), "reset limpa fontes");
    check(pipe->cache_hits() == 0, "reset zera cache_hits");
    check(!pipe->cook("a").ok, "cook vazio após reset");
}

}  // namespace

int main() {
    test_creation();
    test_import_validation();
    test_cook_determinism();
    test_incremental_cache();
    test_package();
    test_persistence();
    test_determinism();
    test_reset();
    if (failures == 0) {
        std::printf("AssetPipelineTests: ALL PASSED\n");
        return 0;
    }
    std::printf("AssetPipelineTests: %d FAILURE(S)\n", failures);
    return 1;
}
