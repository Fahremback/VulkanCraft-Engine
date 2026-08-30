#pragma once
// IClientPrediction — prediction, reconciliation e rollback (seção G).
// Determinístico e transport-free. O cliente tem:
//   • um stream de inputs com sequence/tick/ack (G.1);
//   • predição local por um step injetado (o chamador liga o character
//     controller REAL aqui; o default integra cinematicamente) (G.2);
//   • reconciliação com replay dos inputs não confirmados (G.3);
//   • buffer de snapshots com interpolação/extrapolação (G.4);
//   • predição transacional de block place/break com rollback (G.5);
//   • timeline de rollback sem reexecução incorreta de efeitos (G.7).
//
// Self-contained (std). Os efeitos de bloco são APLICADOS pelo chamador; aqui
// cada edit predito é registrado com estado-before keeping uma trilha para o
// rollback, para que o servidor, ao rejeitar, restaure o mundo (transacional).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace networking {

// Input do cliente em um tick. `sequence` é global crescente; `dt` em segundos.
struct PredictionInput {
    std::uint32_t sequence{ 0 };
    float dt{ 0.0f };
    float move_x{ 0.0f };
    float move_z{ 0.0f };
    bool jump{ false };
};

// Pose predita do controlador (origem do jogador).
struct PredictedPose {
    double x{ 0.0 };
    double y{ 0.0 };
    double z{ 0.0 };
    float yaw{ 0.0f };
};

// Estado autoritativo de uma entidade remota (para o buffer frequent).
struct RemoteSnapshot {
    std::uint64_t entity_net_id{ 0 };
    std::uint64_t tick{ 0 };
    double server_time{ 0.0 };
    PredictedPose pose;
};

// Resultado de reconciliação.
struct ReconcileResult {
    bool corrected{ false };
    float error{ 0.0f };
    std::size_t replayed_inputs{ 0 };
};

// Block edit predito com trilha de rollback (G.5). `block_before` é gravado
// ANTES da aplicação local para permitir restauração transacional.
enum class BlockEditKind : std::uint8_t { Place, Break };
struct BlockPrediction {
    std::uint32_t sequence{ 0 };
    BlockEditKind kind{ BlockEditKind::Place };
    int x{ 0 };
    int y{ 0 };
    int z{ 0 };
    std::uint32_t block_before{ 0 };
    std::uint32_t block_after{ 0 };
    bool server_accepted{ false };   // definitivo quando confirmado/rejeitado
};

// Sinal de rollback entregue ao chamador para este edit (transformação
// transacional: restaurar `block_before` no (x,y,z)).
struct RollbackSignal {
    std::uint32_t sequence{ 0 };
    BlockEditKind kind{ BlockEditKind::Place };
    int x{ 0 };
    int y{ 0 };
    int z{ 0 };
    std::uint32_t restore_block{ 0 };
};

// Step de integração do controlador real (o default constrói uma esteira
// cinemática determinística). Estado + input -> novo estado.
using PredictionStep = std::function<PredictedPose(const PredictedPose&, const PredictionInput&)>;

class IClientPrediction {
public:
    virtual ~IClientPrediction() = default;

    // G.2: define/injeta o step (ligar aqui o ICharacterController real).
    virtual void set_step(PredictionStep step) = 0;

    // G.1/G.2: registra um input, aplica a predição local e devolve o envelope
    // a ser enviado ao servidor.
    virtual PredictionInput predict(float dt, float move_x, float move_z,
                                    bool jump) = 0;

    // Acks processados pelo servidor (libera inputs confirmados da fila).
    virtual void server_ack(std::uint32_t acked_sequence) = 0;

    // G.3: reconcilia com o estado autoritativo; se o erro > threshold, faz
    // snapshot + REPLAY dos inputs ainda não confirmados (reconciliation).
    virtual ReconcileResult reconcile(const PredictedPose& authoritative,
                                      std::uint32_t acked_sequence) = 0;

    // G.4: buffer de snapshots remotos ordenado por tick; amostra o pose no
    // render time (interpolação), extrapolando por veloc. breve.
    virtual bool push_remote_snapshot(const RemoteSnapshot& snapshot) = 0;
    virtual bool sample_remote(std::uint64_t entity_net_id,
                               double render_time, PredictedPose& out) = 0;

    // G.5: predição TRANSACIONAL de block edit. `predict_block` registra a
    // trilha (com block_before) e devolve o sequence para o cliente aplicar o
    // edit localmente. `confirm_block(sequence, accepted)` finaliza: se
    // rejeitado, o edit vira RollbackSignal para o chamador restaurar o mundo.
    virtual std::uint32_t predict_block(BlockEditKind kind, int x, int y, int z,
                                        std::uint32_t block_before,
                                        std::uint32_t block_after) = 0;
    virtual bool confirm_block(std::uint32_t sequence, bool accepted) = 0;

    // G.7: entrega os rollbacks aguardando aplicação no mundo (transacional).
    virtual std::vector<RollbackSignal> drain_rollbacks() = 0;

    // Estado.
    virtual const PredictedPose& pose() const = 0;
    virtual std::uint32_t next_sequence() const = 0;
    virtual std::size_t pending_input_count() const = 0;
    virtual std::size_t pending_block_edits() const = 0;
    virtual bool reset(std::string& errorOut) = 0;
};

std::unique_ptr<IClientPrediction> create_client_prediction(std::string& errorOut);

}  // namespace networking
}  // namespace engine