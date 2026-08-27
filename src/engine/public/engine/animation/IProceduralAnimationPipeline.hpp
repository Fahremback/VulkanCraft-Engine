#pragma once
// IProceduralAnimationPipeline — contrato público de PIPELINE de animação
// procedural de corpo inteiro (agente 4, unidade de integração §10 l.169 —
// "Criar pipeline de animação procedural com IK de corpo inteiro, foot
// placement, look-at, aim e constraints").
//
// Composição (sem duplicar os irmãos):
//   - IFootTerrainSampler → terreno real (mesmo seam do IFootPlacer);
//   - IIkSolver            → look-at/aim (solve_aim) dos ossos de aim;
//   - IConstraints         → limites de articulação por osso (apply_constraint);
//   - o pipeline           → dono da ORQUESTRAÇÃO: alvos de pé ancorados no
//     terreno (foot placement), janela de passo (step window), IK de membros
//     por shortest-arc (IK de corpo inteiro) e a ordem das etapas.
//
// Etapas (ordem determinística, cada uma reporta diagnóstico):
//   StageLegs  → computa os alvos de pé em WORLD: posição do corpo + offset
//                do quadril rotacionado pelo yaw; pé ancorado na superfície
//                (terreno) quando disponível.
//   StageFeet  → janela de passo: se o alvo vertical pulou mais que
//                `footStepLimit` desde a pose de entrada, clamp + `stepLimited`
//                (nunca teleporta).
//   StageIk    → resolve CADA cadeia (leg/arm) por shortest-arc: alinha o
//                eixo origem→alvo com origem→efetor do alvo, escrevendo
//                rotações locais no osso do meio e do fim.
//   StageAim   → look-at/aim dos ossos de aim para `aimTarget` via IIkSolver.
//   StageConstraints → clampa ângulos locais nos limites por osso via
//                IConstraints.
//
// O pipeline é um LAYER sobre uma pose animada de base: `inputPose` (bone →
// transform local) é a entrada; a saída `finalPose` tem os ossos tocados pelas
// etapas ativas ajustados e os demais intactos (ordem preservada).
//
// Determinístico: mesmas entradas → mesma pose final bit-exata entre
// instâncias. Sem RNG, sem relógio de parede, sem estado mutável (o pipeline
// não guarda estado entre run() — cada run é uma função pura). Self-contained
// (std), tipos vetoriais de IAnimCore.

#include "engine/animation/IAnimCore.hpp"
#include "engine/animation/IConstraints.hpp"
#include "engine/animation/IFootPlacement.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

enum class PipelineStage {
    Legs,          // alvos de pé em world
    Feet,          // janela de passo
    Ik,            // shortest-arc por cadeia
    Aim,           // look-at/aim
    Constraints,   // clamp de limites
};

// Qual membro o pipeline resolve.
enum class IkChainKind {
    Leg,     // cadeia de perna (origin=hip, mid=knee, end=foot)
    Arm,     // cadeia de braço (origin=shoulder, mid=elbow, end=hand)
};

struct PipelineIkChain {
    IkChainKind kind = IkChainKind::Leg;
    std::string rootBone;   // osso da origem (hip/shoulder) — toca no StageIk
    std::string midBone;    // osso do meio (knee/elbow) — StageIk escreve
    std::string endBone;    // osso do fim (foot/hand) — StageIk escreve
    // Offset do quadril/ombro em ESPAÇO DO CORPO (Y-up): o pipeline roda o
    // corpo (yaw) e soma à posição do corpo para obter a origem world.
    AnimVec3 jointOffset;
    // Offset de descanso do efetor em espaço do corpo (a posição "parada" do
    // pé/mão quando o agente não anda) — o alvo de StageLegs parte daqui.
    AnimVec3 restOffset;
};

// Configuração do pipeline. `load_from_json`/`validate` all-or-nothing.
struct ProceduralAnimationSpec {
    // Cadeias de membros a resolver (vazio = sem StageIk; Legs/Feet seguem
    // apenas para as cadeias Leg presentes).
    std::vector<PipelineIkChain> chains;
    // Ossos do aim (cabeça/torso); vazio = sem look-at.
    std::vector<std::string> aimBones;
    // Limites por osso aplicados no final (mesmo formato do IConstraints).
    std::vector<JointLimit> jointLimits;
    // Janela de passo vertical (m) — quanto o alvo de pé pode descer/subir
    // por frame em relação à pose de entrada. > 0.
    double footStepLimit = 0.5;
    // Etapas ativas (todas por default). O caller pode desligar etapas para
    // isolar o efeito (ex.: só Ik, sem Constraints).
    bool stageLegs = true;
    bool stageFeet = true;
    bool stageIk = true;
    bool stageAim = true;
    bool stageConstraints = true;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

// Estado do corpo no frame atual (entrada do pipeline).
struct PipelineBodyState {
    AnimVec3 position;        // origem do corpo em world space
    float yawRadians = 0.0f;  // heading (radianos em torno de Y)
    AnimVec3 aimTarget;       // alvo do look-at (world space)
    bool hasAimTarget = false;
};

// Resultado por etapa — diagnóstico de onde o pipeline ajustou.
struct StageDiagnostic {
    PipelineStage stage = PipelineStage::Legs;
    std::uint32_t bonesTouched = 0;   // ossos alterados na etapa
    bool stepLimited = false;         // StageFeet clampou um alvo de pé
    std::string message;              // detalhe
};

// Um membro resolvido (para diagnóstico/consumo).
struct ResolvedEffector {
    IkChainKind kind = IkChainKind::Leg;
    std::string endBone;
    AnimVec3 targetWorld;    // alvo world usado no solve
    double reachDistance = 0.0;  // |origem - alvo|
};

// Resultado do pipeline: pose final + membros + diagnósticos.
struct ProceduralAnimationResult {
    std::vector<BonePose> finalPose;        // ordem preservada da entrada
    std::vector<ResolvedEffector> effectors;
    std::vector<StageDiagnostic> diagnostics;  // por etapa, em ordem
    bool ok = false;
};

// Pipeline de animação procedural de corpo inteiro (determinístico).
class IProceduralAnimationPipeline {
public:
    virtual ~IProceduralAnimationPipeline() = default;

    // Aplica a spec (all-or-nothing via ProceduralAnimationSpec::validate).
    virtual bool configure(const ProceduralAnimationSpec& spec,
                           std::string& errorOut) = 0;

    // Roda o pipeline sobre a pose de origem. `terrain` pode ser null quando
    // stageLegs/stageFeet estão desligados (ou nenhuma cadeia Leg existe).
    // Erro real (pose sem um osso referenciado, spec inválida) recusa com
    // `out.ok == false` e diagnóstico preenchido.
    virtual ProceduralAnimationResult run(
        const PipelineBodyState& body, const IFootTerrainSampler* terrain,
        const std::vector<BonePose>& inputPose, std::string& errorOut) = 0;

    // Estado serializado bit-exact (spec, nada mutável) / restaurado.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IProceduralAnimationPipeline).
std::unique_ptr<IProceduralAnimationPipeline>
create_procedural_animation_pipeline();

}  // namespace engine::animation
