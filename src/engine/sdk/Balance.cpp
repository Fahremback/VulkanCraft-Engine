// Balance.cpp — adapter do contrato IBalance (engine::gameplay).
// Polígono de suporte = fecho convexo dos pontos (Andrew monotone chain);
// CoM dentro do polígono → Stable (correção 0); na borda (margem <=
// edgeMargin) → Edge com correção de tornozelo; fora → Unstable com
// correção de quadril. Tudo determinístico (sem RNG, ordem fixa).

#include "engine/gameplay/IBalance.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine::gameplay {

namespace {

struct Pt {
    float x;
    float z;
};

// Produto vetorial 2D (z de (a-b) x (c-b)): > 0 = curva anti-horária.
float cross(const Pt& o, const Pt& a, const Pt& b) {
    return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x);
}

std::vector<Pt> convex_hull(std::vector<Pt> pts) {
    std::sort(pts.begin(), pts.end(),
              [](const Pt& a, const Pt& b) {
                  if (a.x != b.x) return a.x < b.x;
                  return a.z < b.z;
              });
    if (pts.size() < 3) return pts;  // degenerado: sem polígono
    std::vector<Pt> hull;
    // Lower hull.
    for (const Pt& p : pts) {
        while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull.back(), p) <= 0.0f)
            hull.pop_back();
        hull.push_back(p);
    }
    // Upper hull (Andrew canônico): percorre do penúltimo ao primeiro — o
    // último ponto (índice size-1) já fechou o lower hull, então começar nele
    // duplicaria o vértice. Ao final, pop_back remove o primeiro repetido.
    const std::size_t lowerSize = hull.size();
    // Andrew canônico: upper hull percorre do penúltimo (size-2) ao
    // primeiro (0) — o último ponto já fechou o lower hull.
    for (std::size_t n = pts.size() - 1; n-- > 0;) {
        while (hull.size() > lowerSize &&
               cross(hull[hull.size() - 2], hull.back(), pts[n]) <= 0.0f)
            hull.pop_back();
        hull.push_back(pts[n]);
    }
    hull.pop_back();  // último = primeiro (duplicata)
    return hull;
}

// Ponto dentro do polígono convexo (sentido anti-horário) por cross products.
bool inside_convex(const std::vector<Pt>& polygon, float x, float z) {
    const Pt p{ x, z };
    bool sign = false;
    for (std::size_t n = 0; n < polygon.size(); ++n) {
        const Pt& a = polygon[n];
        const Pt& b = polygon[(n + 1) % polygon.size()];
        const float c = cross(a, b, p);
        if (c == 0.0f) continue;
        if (!sign) sign = c > 0.0f;
        else if ((c > 0.0f) != sign) return false;
    }
    return true;
}

// Distância com sinal do ponto à borda do polígono convexo: positiva =
// dentro, negativa = fora. Usa a NORMAL ESQUERDA de cada aresta (polígono
// anti-horário → interior à esquerda, sinal positivo) e retorna a MÍNIMA
// sobre as arestas = distância à aresta MAIS PRÓXIMA (a mais crítica para o
// equilíbrio).
float edge_margin(const std::vector<Pt>& polygon, float x, float z) {
    float best = 1.0e30f;
    for (std::size_t n = 0; n < polygon.size(); ++n) {
        const Pt& a = polygon[n];
        const Pt& b = polygon[(n + 1) % polygon.size()];
        const float abx = b.x - a.x;
        const float abz = b.z - a.z;
        const float len = std::sqrt(abx * abx + abz * abz);
        if (len < 1.0e-8f) continue;
        // cross(a, b, p) / len = distância com sinal à aresta — MESMA
        // convenção do inside_convex (interior CCW → positivo).
        const float signedDist = (abx * (z - a.z) - abz * (x - a.x)) / len;
        best = std::min(best, signedDist);
    }
    return best;
}

}  // namespace

class BalanceImpl final : public IBalance {
public:
    void set_config(const BalanceConfig& config) override { config_ = config; }
    BalanceConfig config() const override { return config_; }

    BalanceResult evaluate(float comX, float comZ,
                           const SupportPoint* points,
                           std::size_t pointCount) override {
        BalanceResult result;
        if (points == nullptr || pointCount < 3) {
            result.state = BalanceState::Unstable;  // sem suporte
            return result;
        }
        std::vector<Pt> pts;
        pts.reserve(pointCount);
        for (std::size_t n = 0; n < pointCount; ++n) {
            pts.push_back(Pt{ points[n].x, points[n].z });
        }
        const std::vector<Pt> hull = convex_hull(pts);
        if (hull.size() < 3) {
            result.state = BalanceState::Unstable;  // degenerado
            return result;
        }
        const bool inside = inside_convex(hull, comX, comZ);
        const float margin = edge_margin(hull, comX, comZ);
        result.margin = margin;

        if (inside) {
            if (margin <= config_.edgeMargin) {
                result.state = BalanceState::Edge;
                const float error = config_.edgeMargin - margin;
                apply_correction(result, config_.ankleGain * error,
                                 comX, comZ, hull);
            } else {
                result.state = BalanceState::Stable;
            }
        } else {
            result.state = BalanceState::Unstable;
            // Correção de quadril: empurra o CoM de volta para dentro.
            const float error = -margin;
            apply_correction(result, config_.hipGain * error,
                             comX, comZ, hull);
        }
        return result;
    }

private:
    // Correção na direção CoM → centróide do polígono de suporte
    // (determinística). Magnitude limitada por maxCorrection.
    void apply_correction(BalanceResult& result, float magnitude,
                          float comX, float comZ,
                          const std::vector<Pt>& hull) const {
        if (magnitude > config_.maxCorrection) magnitude = config_.maxCorrection;
        float cx = 0.0f;
        float cz = 0.0f;
        for (const Pt& p : hull) { cx += p.x; cz += p.z; }
        cx /= static_cast<float>(hull.size());
        cz /= static_cast<float>(hull.size());
        float dx = cx - comX;
        float dz = cz - comZ;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len > 1.0e-6f) { dx /= len; dz /= len; }
        else { dx = 0.0f; dz = 0.0f; }  // CoM no centróide: sem direção
        result.correctionX = dx * magnitude;
        result.correctionZ = dz * magnitude;
    }

    BalanceConfig config_;
};

std::unique_ptr<IBalance> create_balance() {
    return std::make_unique<BalanceImpl>();
}

}  // namespace engine::gameplay
