// IDayNightCycle — relógio diurno/noturno DETERMINÍSTICO. Componente CORE do
// §3 item 37 ("integrar IA a ... mundo voxel, day/night e streaming"): os
// sistemas (IA, mobs, iluminação, áudio) consultam o relógio; a simulação
// avança por `advance(dt)` — o mesmo dt aplicado na mesma ordem produz o
// mesmo estado (nada de relógio de parede). `time_of_day()` é a fração do
// ciclo em [0,1) (0 = meia-noite, 0.25 = nascer, 0.5 = meio-dia, 0.75 =
// pôr); `sun_altitude()` é o seno da elevação do sol em [-1,1];
// `daylight_factor()` é uma transição suave 0..1 (noite→dia) determinística.
// O estado é serializável (to_json) p/ replay/persistência.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace engine {
namespace gameplay {

struct DayNightConfig {
    float dayLengthSeconds{ 1200.0f };  // duração do ciclo completo (> 0)
    float startOfDay{ 0.0f };           // fração do ciclo no t=0, em [0,1)
};

class IDayNightCycle {
public:
    virtual ~IDayNightCycle() = default;

    // All-or-nothing: dayLengthSeconds <= 0 ou não-finito, startOfDay fora de
    // [0,1) → recusa e mantém a config anterior.
    virtual bool configure(const DayNightConfig& config, std::string& errorOut) = 0;

    // Avança o relógio. dt < 0 ou não-finito é no-op (determinismo).
    virtual void advance(float dt) = 0;

    // Busca determinística: seta a fração do ciclo (wrap em [0,1)).
    virtual void seek(float timeOfDay) = 0;

    virtual float time_of_day() const = 0;     // [0,1)
    virtual float sun_altitude() const = 0;    // [-1,1]
    virtual float daylight_factor() const = 0;  // [0,1]

    // Estado serializado (%.9g) p/ replay/persistência bit-exact.
    virtual std::string to_json() const = 0;
    virtual bool load_from_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
};

std::unique_ptr<IDayNightCycle> create_day_night_cycle();

}  // namespace gameplay
}  // namespace engine
