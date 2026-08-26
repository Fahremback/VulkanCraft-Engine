#pragma once
// IInertializer — inertialização de animação (transições sem snap).
//
// Contrato público self-contained (std only) do §4 item 46 (componente
// "inertialization"). Sem dependência de core — só os tipos vetoriais de
// IAnimCore. Implementa o envelope clássico de decaimento criticamente
// amortecido: no reset, o resíduo (diferença entre a pose interrompida e o
// novo alvo) é capturado; a cada tick ele decai com d(t) = (1 + t/T)·e^(−t/T)
// e a saída = alvo ⊕ resíduo·d — a pose continua suave e converge ao alvo.
//
// Semântica determinística:
//   reset(current, target): resíduo = current ⊖ target por osso —
//     pos = c.pos − t.pos; rot = c.rot · t.rot⁻¹; scale = c.scale / t.scale.
//     All-or-nothing: mesmos ossos, mesma ordem.
//   tick(target, dt): t += dt; d = (1 + t/T)·e^(−t/T);
//     out = target ⊕ resíduo·d (pos soma; rot = t.rot · slerp(I, res, d);
//     scale = t.scale · lerp(1, res, d)). settled = t ≥ 4·T.
//   clear(): zera resíduo e relógio (tick devolve o alvo intacto).

#include "engine/animation/IAnimCore.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

struct InertializerResult {
    std::vector<BonePose> pose;
    bool settled = false;
};

// Inertializador determinístico (sem RNG/relógio real).
class IInertializer {
public:
    virtual ~IInertializer() = default;

    // Tempo de decaimento T (segundos, > 0). O envelope (1 + t/T)·e^(−t/T)
    // usa T p/ dimensionar a suavidade.
    virtual void set_decay_time(double seconds, std::string& errorOut) = 0;
    virtual double decay_time() const = 0;

    // Captura o resíduo da descontinuidade (current ⊖ target).
    virtual bool reset(const std::vector<BonePose>& current,
                       const std::vector<BonePose>& target,
                       std::string& errorOut) = 0;

    // Avança o relógio e devolve target ⊕ resíduo·d(t). Sem resíduo ativo,
    // devolve o alvo intacto com settled = true.
    virtual InertializerResult tick(const std::vector<BonePose>& target,
                                    double dt, std::string& errorOut) = 0;

    virtual void clear() = 0;
    virtual bool is_active() const = 0;

    // Registro completo serializado / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IInertializer).
std::unique_ptr<IInertializer> create_inertializer();

}  // namespace engine::animation
