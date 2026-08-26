#pragma once
// IPublishPipeline — contrato público do pipeline de publicação do editor
// (agente 2 §C, "import→cook→inspect→edit→play→package→publish").
//
// Máquina de estágios determinística do fluxo de build do editor: o editor
// dirige o contrato a cada etapa do build_game() real (cook → package → scene
// → shaders → done) e o contrato valida as transições, acumula contadores e
// expõe um JSON estável para observabilidade (GET /publish). SEM RNG, SEM
// relógio de parede, SEM estado global. Self-contained (std apenas).

#include <memory>
#include <string>

namespace engine::editor {

// Estágios do pipeline (ordem canônica do build).
enum class PublishStage {
    Idle,        // nenhum build em andamento
    Cooking,     // cozinhando assets não-cozidos
    Packaging,   // empacotando assets cozidos
    Publishing,  // salvando cena inicial + copiando shaders
    Done,        // build concluído com sucesso
    Failed,      // build falhou (motivo em last_error)
};

// Estado observável do pipeline.
struct PublishState {
    PublishStage stage = PublishStage::Idle;
    size_t imported = 0;   // assets cozidos nesta execução
    size_t failed = 0;     // assets que falharam no cook
    size_t packaged = 0;   // assets empacotados
    std::string last_error;  // motivo da falha (vazio quando ok)
    std::string project;     // nome do projeto do último build (vazio se nenhum)
};

// Contrato do pipeline de publicação.
struct IPublishPipeline {
    virtual ~IPublishPipeline() = default;

    virtual PublishState state() const = 0;

    // Inicia um build: Idle → Cooking. Recusa (false, estado inalterado)
    // quando já está em andamento. all-or-nothing.
    virtual bool begin(const std::string& project) = 0;

    // Transições de estágio: Cooking → Packaging → Publishing → Done.
    // Cada transição só é válida a partir do estágio anterior na ordem
    // canônica; inválida → false sem mutar.
    virtual bool cooking_done(size_t imported, size_t failed) = 0;  // → Packaging
    virtual bool packaging_done(size_t packaged) = 0;               // → Publishing
    virtual bool publishing_done() = 0;                             // → Done

    // Falha a qualquer momento do build (Cooking/Packaging/Publishing) → Failed.
    virtual bool fail(const std::string& error) = 0;

    // Reseta para Idle (contadores zerados, erro limpo).
    virtual void reset() = 0;

    // JSON determinístico: {"stage","imported","failed","packaged",
    // "project","error"}.
    virtual std::string to_json() const = 0;
};

// Factory do adapter (implementada em src/engine/sdk/PublishPipeline.cpp).
std::unique_ptr<IPublishPipeline> create_publish_pipeline();

}  // namespace engine::editor
