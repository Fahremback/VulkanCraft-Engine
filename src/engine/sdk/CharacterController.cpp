// CharacterController.cpp — adapter do contrato ICharacterController
// (engine::gameplay). Resolução determinística de movimento sobre terreno:
// amostra de chão mais próxima do alvo, step-up (<= maxStepHeight),
// step-down/snap (<= stepDownDistance), projeção de inclinação acima do
// limite e água (arrasto exponencial + flutuação linear).

#include "engine/gameplay/ICharacterController.hpp"

#include <algorithm>
#include <cmath>

namespace engine::gameplay {

namespace {

float radians(float degrees) { return degrees * 3.14159265358979323846f / 180.0f; }

}  // namespace

class CharacterControllerImpl final : public ICharacterController {
public:
    void set_config(const CharacterConfig& config) override { config_ = config; }
    CharacterConfig config() const override { return config_; }

    MoveResult move(float fromX, float fromY, float fromZ,
                    float targetX, float targetZ,
                    const TerrainSample* samples, std::size_t sampleCount) override {
        MoveResult result;

        // Sem amostras: sem chão — queda livre limitada pelo snap máximo.
        if (sampleCount == 0) {
            result.newY = fromY - config_.stepDownDistance;
            result.snappedDown = true;
            result.inWater = config_.waterSurfaceY > result.newY;
            return result;
        }

        // Amostra de chão mais próxima do alvo (XZ).
        const TerrainSample* ground = samples;
        float best = squared_distance(ground->x, ground->z, targetX, targetZ);
        for (std::size_t n = 1; n < sampleCount; ++n) {
            const float d = squared_distance(samples[n].x, samples[n].z, targetX, targetZ);
            if (d < best) {
                best = d;
                ground = &samples[n];
            }
        }
        const float groundY = ground->height;

        // Step-up: só sobe se o degrau couber no limite.
        const float rise = groundY - fromY;
        if (rise > config_.maxStepHeight + 1.0e-5f) {
            result.slopeBlocked = true;  // inclinação ou parede acima do limite
            result.newY = fromY;
            result.inWater = config_.waterSurfaceY > result.newY;
            return result;
        }
        if (rise > 1.0e-5f) {
            result.steppedUp = true;
            result.newY = groundY;
        } else {
            // Step-down / snap: desce até a distância de snap; se o chão
            // estiver mais fundo, desce só o snap (não atravessa vãos).
            const float drop = fromY - groundY;
            result.newY = (drop <= config_.stepDownDistance + 1.0e-5f)
                              ? groundY
                              : fromY - config_.stepDownDistance;
            result.snappedDown = result.newY < fromY - 1.0e-5f;
        }

        // Inclinação: distância horizontal percorrida (from→target) define a
        // rampa. Se rise/dx ultrapassar o limite, projeta a altura em
        // maxSlope (sobe menos do que o terreno íngreme sugere — sem
        // teleporte para o topo).
        const float dx = std::sqrt((targetX - fromX) * (targetX - fromX) +
                                   (targetZ - fromZ) * (targetZ - fromZ));
        if (dx > 1.0e-5f && rise > 1.0e-5f) {
            const float slopeDeg = std::atan2(rise, dx) * 180.0f / 3.14159265358979323846f;
            if (slopeDeg > config_.maxSlopeDegrees) {
                result.slopeBlocked = true;
                result.newY = fromY + dx * std::tan(radians(config_.maxSlopeDegrees));
                if (result.newY < fromY) result.newY = fromY;
            }
        }

        result.inWater = config_.waterSurfaceY > result.newY;
        return result;
    }

    float water_velocity(float velocityY, float depth, float dt) const override {
        if (depth <= 0.0f || dt <= 0.0f) return velocityY;
        // Arrasto exponencial: v *= e^(-drag*dt). Flutuação: acelera para
        // cima proporcional à profundidade (determinístico, sem RNG).
        const float damped = velocityY * std::exp(-config_.waterDrag * dt);
        return damped + config_.waterBuoyancy * depth * dt;
    }

private:
    static float squared_distance(float ax, float az, float bx, float bz) {
        const float ddx = ax - bx;
        const float ddz = az - bz;
        return ddx * ddx + ddz * ddz;
    }

    CharacterConfig config_;
};

std::unique_ptr<ICharacterController> create_character_controller() {
    return std::make_unique<CharacterControllerImpl>();
}

}  // namespace engine::gameplay
