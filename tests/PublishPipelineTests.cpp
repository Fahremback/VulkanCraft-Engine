// PublishPipelineTests — gate headless do contrato engine/editor IPublishPipeline.
//
// Verifica a máquina de estágios do build: transições válidas na ordem
// canônica, recusas all-or-nothing de transições inválidas/saltos, contadores
// acumulando, falha em qualquer estágio, reset e JSON determinístico.

#include "engine/editor/IPublishPipeline.hpp"

#include <cstdio>
#include <string>

using engine::editor::create_publish_pipeline;
using engine::editor::PublishStage;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void test_begin_from_idle() {
    auto p = create_publish_pipeline();
    check(p->state().stage == PublishStage::Idle, "estado inicial = Idle");
    check(p->begin("demo"), "begin aceito a partir de Idle");
    check(p->state().stage == PublishStage::Cooking, "begin → Cooking");
    check(p->state().project == "demo", "begin registra o projeto");
}

void test_begin_refuses_in_progress() {
    auto p = create_publish_pipeline();
    p->begin("a");
    check(!p->begin("b"), "begin recusa build em andamento (Cooking)");
    check(p->state().project == "a", "projeto inalterado após recusa");
}

void test_canonical_order() {
    auto p = create_publish_pipeline();
    p->begin("demo");
    check(!p->packaging_done(5), "packaging_done recusado antes do cook");
    check(p->state().stage == PublishStage::Cooking, "estágio inalterado após salto");
    check(p->cooking_done(10, 2), "cooking_done válido → Packaging");
    check(p->state().imported == 10 && p->state().failed == 2, "contadores do cook");
    check(!p->publishing_done(), "publishing_done recusado antes do package");
    check(p->packaging_done(9), "packaging_done válido → Publishing");
    check(p->state().packaged == 9, "contador de empacotados");
    check(p->publishing_done(), "publishing_done válido → Done");
    check(p->state().stage == PublishStage::Done, "estágio final = Done");
}

void test_fail_any_stage() {
    auto p = create_publish_pipeline();
    check(!p->fail("nada em andamento"), "fail recusado em Idle");
    p->begin("demo");
    check(p->fail("cook exploded"), "fail em Cooking → Failed");
    check(p->state().stage == PublishStage::Failed, "estágio = Failed");
    check(p->state().last_error == "cook exploded", "erro registrado");
    // falha também a partir de Packaging
    auto q = create_publish_pipeline();
    q->begin("demo");
    q->cooking_done(1, 0);
    check(q->fail("no assets"), "fail em Packaging → Failed");
    // a partir de Done não falha
    auto r = create_publish_pipeline();
    r->begin("demo");
    r->cooking_done(0, 0);
    r->packaging_done(0);
    r->publishing_done();
    check(!r->fail("tarde demais"), "fail recusado em Done");
}

void test_reset() {
    auto p = create_publish_pipeline();
    p->begin("demo");
    p->cooking_done(7, 1);
    p->reset();
    check(p->state().stage == PublishStage::Idle, "reset → Idle");
    check(p->state().imported == 0 && p->state().failed == 0 &&
              p->state().packaged == 0, "contadores zerados");
    check(p->state().last_error.empty(), "erro limpo");
    check(p->begin("again"), "begin funciona após reset");
}

void test_json_deterministic() {
    auto a = create_publish_pipeline();
    auto b = create_publish_pipeline();
    check(a->to_json() == b->to_json(), "JSON idêntico entre instâncias");
    a->begin("demo");
    a->cooking_done(3, 0);
    a->packaging_done(3);
    const std::string j = a->to_json();
    check(j.find("\"stage\":\"publishing\"") != std::string::npos, "JSON contém estágio");
    auto c = create_publish_pipeline();
    c->begin("demo");
    c->cooking_done(3, 0);
    c->packaging_done(3);
    check(c->to_json() == j, "JSON determinístico para a mesma sequência");
}

}  // namespace

int main() {
    test_begin_from_idle();
    test_begin_refuses_in_progress();
    test_canonical_order();
    test_fail_any_stage();
    test_reset();
    test_json_deterministic();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
