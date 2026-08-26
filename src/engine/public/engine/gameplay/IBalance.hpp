#pragma once

// IBalance — modelo determinístico de equilíbrio (apoio, centro de massa,
// estratégia de recuperação). Unidade do §4 item 47 (equilíbrio) — o resto
// do item (locomoção/apoio/alcance/re ação/pose) é coberto por IProceduralLegs
// + IGaitPlanner + IPoseWarper (AGENT-4/animação).
//
// O contrato NÃO conhece física: recebe a projeção do centro de massa (XZ),
// o polígono de suporte (pés plantados) e a velocidade atual; classifica o
// estado (estável / na borda / instável) e devolve a CORREÇÃO necessária —
// estratégia de tornozelo (anterior) ou de quadril (posterior) com ganho
// determinístico — que o chamador aplica onde quiser (física real, kinematic,
// pose). Sem RNG, sem estado global.

#include <cstddef>
#include <memory>

namespace engine::gameplay {

enum class BalanceState {
    Stable,      // CoM bem dentro do polígono de suporte
    Edge,        // CoM na borda (correção de tornozelo)
    Unstable,    // CoM fora do polígono (correção de quadril + passo)
};

struct BalanceConfig {
    float edgeMargin{ 0.05f };   // faixa "na borda" antes de instável (>= 0)
    float ankleGain{ 8.0f };     // correção de tornozelo por unidade de erro (>= 0)
    float hipGain{ 3.0f };       // correção de quadril por unidade de erro (>= 0)
    float maxCorrection{ 2.0f }; // teto da correção devolvida (>= 0)
};

// Um vértice do polígono de suporte (pé plantado), em XZ.
struct SupportPoint {
    float x{ 0.0f };
    float z{ 0.0f };
};

struct BalanceResult {
    BalanceState state{ BalanceState::Stable };
    float correctionX{ 0.0f };  // correção a aplicar no CoM (XZ)
    float correctionZ{ 0.0f };
    float margin{ 0.0f };       // distância do CoM à borda do polígono
                                // (positiva = dentro, negativa = fora)
};

class IBalance {
public:
    virtual ~IBalance() = default;

    virtual void set_config(const BalanceConfig& config) = 0;
    virtual BalanceConfig config() const = 0;

    // Avalia o equilíbrio. `comX/comZ` é a projeção do centro de massa,
    // `points` o polígono de suporte (>= 3 pontos, ordem arbitrária).
    // Retorna o estado e a correção determinística.
    virtual BalanceResult evaluate(float comX, float comZ,
                                   const SupportPoint* points,
                                   std::size_t pointCount) = 0;
};

std::unique_ptr<IBalance> create_balance();

}  // namespace engine::gameplay
