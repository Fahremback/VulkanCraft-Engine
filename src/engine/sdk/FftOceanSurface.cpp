// FftOceanSurface.cpp — Agente 1 (task_plan C.20 vkfft — NOVO, criado do zero
// em código nativo), the deterministic Gaussian ocean heightfield core that
// closes the ocean pathway of the FFT seam. Uses the native IFftCore for the
// 2D inverse transform (2 passadas 1D). No external vendor GPU; pure std+glm.
#include "engine/rendering/IFftOceanSurface.hpp"
#include "engine/rendering/IFftCore.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace Engine::Rendering {

namespace {

// deterministic split-mix64 PRNG (no <random> clock state)
std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
float unitFloat(std::uint64_t s) noexcept {
    return static_cast<float>((s >> 32) * (1.0 / 4294967296.0));
}

bool parseJson(const std::string& json, FftOceanConfig& out, std::string& err) {
    auto getField = [&json](const char* key) -> std::string {
        std::string needle = std::string("\"") + key + "\"";
        std::size_t pos = json.find(needle);
        if (pos == std::string::npos) return {};
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return {};
        std::string v;
        pos++;  // skip ':'
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        if (pos < json.size() && json[pos] == '"') {  // string
            pos++;
            while (pos < json.size() && json[pos] != '"') { v += json[pos]; pos++; }
        } else {  // number
            while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ' ') { v += json[pos]; pos++; }
        }
        return v;
    };
    std::string s;
    s = getField("size"); if (!s.empty()) out.size = static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
    s = getField("tileSizeMeters"); if (!s.empty()) out.tileSizeMeters = std::strtof(s.c_str(), nullptr);
    s = getField("windSpeed"); if (!s.empty()) out.windSpeed = std::strtof(s.c_str(), nullptr);
    s = getField("windDirRad"); if (!s.empty()) out.windDirRad = std::strtof(s.c_str(), nullptr);
    s = getField("choppiness"); if (!s.empty()) out.choppiness = std::strtof(s.c_str(), nullptr);
    s = getField("amplitude"); if (!s.empty()) out.amplitude = std::strtof(s.c_str(), nullptr);
    s = getField("seed"); if (!s.empty()) out.seed = static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
    std::string ver = getField("version");
    if (!ver.empty() && std::strtoul(ver.c_str(), nullptr, 10) != 1u) { err = "version must be 1"; return false; }
    if (!out.valid(err)) return false;
    return true;
}

class FftOceanSurface final : public IFftOceanSurface {
public:
    FftOceanSurface() : config_(FftOceanConfig{}), fft_(create_fft_core(err_)) {}

    bool configure(const FftOceanConfig& config, std::string& errorOut) override {
        if (!config.valid(errorOut)) return false;
        config_ = config;
        return true;
    }
    const FftOceanConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText, std::string& errorOut) override {
        FftOceanConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) return false;
        return configure(parsed, errorOut);
    }
    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"size\": " << config_.size
          << ", \"tileSizeMeters\": " << config_.tileSizeMeters
          << ", \"windSpeed\": " << config_.windSpeed
          << ", \"windDirRad\": " << config_.windDirRad
          << ", \"choppiness\": " << config_.choppiness
          << ", \"amplitude\": " << config_.amplitude
          << ", \"seed\": " << config_.seed << " }";
        return o.str();
    }

    float spectrum(float kx, float kz) const noexcept override {
        const float k = std::sqrt(kx * kx + kz * kz);
        if (k < 1e-6f) return 0.0f;
        const float g = 9.81f;
        const float L = (config_.windSpeed * config_.windSpeed) / g;
        // Phillips spectrum magnitude
        const float k2 = k * k;
        float phillips = (config_.amplitude / (k2 * k2));
        // exp(-1/(kL)^2): waves shorter than L*... cutoff
        const float kl = k * L;
        float pole = std::exp(-1.0f / (kl * kl));
        // directionality (Tessendorf)
        const float wx = std::cos(config_.windDirRad);
        const float wz = std::sin(config_.windDirRad);
        float kxUnit = (k > 1e-6f) ? kx / k : 0.0f;
        float kzUnit = (k > 1e-6f) ? kz / k : 0.0f;
        float dot = kxUnit * wx + kzUnit * wz;
        float dirTerm = dot * dot;
        return phillips * pole * dirTerm;
    }

    bool synthesize(float time, std::vector<FftOceanVertex>& output,
                    std::string& errorOut) const override {
        if (!std::isfinite(time)) { errorOut = "time must be finite"; return false; }
        const std::uint32_t n = config_.size;
        const float L = config_.tileSizeMeters;
        const float g = 9.81f;

        // Tessendorf construction: first draw the initial spectrum h0(k) with
        // deterministic per-cell random phase for the WHOLE plane, then build
        //   H(k,t) = h0(k)·e^{i·ω(k)·t} + conj(h0(−k))·e^{−i·ω(k)·t}
        // which is Hermitian BY CONSTRUCTION: H(−k,t) = conj(H(k,t)) for every
        // k, so the inverse FFT yields a real height field. (The previous code
        // gave every cell an independent phase with no pairing, so the field
        // was not actually Hermitian-symmetric.)
        const std::size_t N = static_cast<std::size_t>(n);
        std::vector<std::complex<float>> H0(N * N), H(N * N);
        std::uint64_t seed = config_.seed;
        const float dk = 2.0f * 3.14159265358979f / L;

        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t x = 0; x < n; ++x) {
                std::int32_t kx = static_cast<std::int32_t>(x);
                std::int32_t kz = static_cast<std::int32_t>(z);
                if (kx > static_cast<std::int32_t>(n / 2)) kx -= static_cast<std::int32_t>(n);
                if (kz > static_cast<std::int32_t>(n / 2)) kz -= static_cast<std::int32_t>(n);
                const float fx = dk * static_cast<float>(kx);
                const float fz = dk * static_cast<float>(kz);
                // Amplitude espectral: |h0| = sqrt(P(k)) (Tessendorf usa a raiz
                // do espectro, não o espectro em si).
                const float en = std::sqrt(std::max(spectrum(fx, fz), 0.0f));
                // deterministic phase (seeded per grid cell)
                std::uint64_t s = mix64(seed + static_cast<std::uint64_t>(z) * 1000003ull +
                                        static_cast<std::uint64_t>(x) * 1031ull + 0x9e3779b97f4a7c15ull);
                const float phi = unitFloat(s) * 2.0f * 3.14159265358979f;
                H0[static_cast<std::size_t>(z) * N + x] =
                    std::complex<float>(std::cos(phi) * en, std::sin(phi) * en);
            }
        }

        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t x = 0; x < n; ++x) {
                std::int32_t kx = static_cast<std::int32_t>(x);
                std::int32_t kz = static_cast<std::int32_t>(z);
                if (kx > static_cast<std::int32_t>(n / 2)) kx -= static_cast<std::int32_t>(n);
                if (kz > static_cast<std::int32_t>(n / 2)) kz -= static_cast<std::int32_t>(n);
                const float fx = dk * static_cast<float>(kx);
                const float fz = dk * static_cast<float>(kz);
                // Deep-water dispersion: omega = sqrt(g·|k|), função só de |k|,
                // então o pareamento Hermitiano é preservado no tempo.
                const float kMag = std::sqrt(fx * fx + fz * fz);
                const float omega = (kMag > 1e-6f) ? std::sqrt(g * kMag) : 0.0f;
                const float wt = omega * time;
                const std::complex<float> tp(std::cos(wt), std::sin(wt));
                // −k na indexação wrap-around (N par): índice (n−x)%n em cada
                // eixo; DC e modo de Nyquist são o próprio conjugado.
                const std::size_t idx = static_cast<std::size_t>(z) * N + x;
                const std::size_t mz = static_cast<std::size_t>((n - z) % n);
                const std::size_t mx = static_cast<std::size_t>((n - x) % n);
                H[idx] = H0[idx] * tp +
                         std::conj(H0[mz * N + mx]) * std::conj(tp);
            }
        }

        // Choppy-waves displacement spectra (Tessendorf): the horizontal
        // displacement is NOT the imaginary part of the height field (that is
        // ~0 because H is Hermitian -> IFFT is real). It is its own spectral
        // field, derived from the height spectrum via the gradient operator:
        //   Dx(k) = i * (-kx/|k|) * H(k)
        //   Dz(k) = i * (-kz/|k|) * H(k)
        // Each is Hermitian BY CONSTRUCTION (D(-k) = conj(D(k)) follows from
        // H(-k) = conj(H(k)) and the real factor k/|k| flipping sign), so each
        // IFFT yields a real displacement field. The three spectra are
        // transformed SEPARATELY (height, Dx, Dz) and only then combined.
        std::vector<std::complex<float>> Dx(N * N), Dz(N * N);
        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t x = 0; x < n; ++x) {
                std::int32_t kx = static_cast<std::int32_t>(x);
                std::int32_t kz = static_cast<std::int32_t>(z);
                if (kx > static_cast<std::int32_t>(n / 2)) kx -= static_cast<std::int32_t>(n);
                if (kz > static_cast<std::int32_t>(n / 2)) kz -= static_cast<std::int32_t>(n);
                const float fx = dk * static_cast<float>(kx);
                const float fz = dk * static_cast<float>(kz);
                const float kMag = std::sqrt(fx * fx + fz * fz);
                const std::size_t idx = static_cast<std::size_t>(z) * N + x;
                if (kMag < 1e-6f) {
                    Dx[idx] = std::complex<float>(0.0f, 0.0f);  // DC: no gradient
                    Dz[idx] = std::complex<float>(0.0f, 0.0f);
                } else {
                    // i * (-k/|k|) * H : the i rotates the gradient into the
                    // horizontal plane (phase-quadrature), the -k/|k| unit
                    // vector points the displacement along the wave normal.
                    const std::complex<float> i(0.0f, 1.0f);
                    const float invK = 1.0f / kMag;
                    Dx[idx] = i * (-fx * invK) * H[idx];
                    Dz[idx] = i * (-fz * invK) * H[idx];
                }
            }
        }

        // 2D inverse FFT via two passes of 1D IFftCore (each includes 1/N,
        // so the composition is 1/N^2 = the 2D inverse scaling). Transform
        // height, Dx and Dz separately.
        const auto ifft2D = [&](const std::vector<std::complex<float>>& src,
                                std::vector<std::complex<float>>& dst) {
            std::vector<std::complex<float>> row(n), rowRes(n), col(n), colRes(n);
            for (std::uint32_t z = 0; z < n; ++z) {
                for (std::uint32_t x = 0; x < n; ++x) row[x] = src[static_cast<std::size_t>(z) * N + x];
                fft_->ifft(row, rowRes);
                for (std::uint32_t x = 0; x < n; ++x) dst[static_cast<std::size_t>(z) * N + x] = rowRes[x];
            }
            for (std::uint32_t x = 0; x < n; ++x) {
                for (std::uint32_t z = 0; z < n; ++z) col[z] = dst[static_cast<std::size_t>(z) * N + x];
                fft_->ifft(col, colRes);
                for (std::uint32_t z = 0; z < n; ++z) dst[static_cast<std::size_t>(z) * N + x] = colRes[z];
            }
        };
        std::vector<std::complex<float>> hField(N * N), dxField(N * N), dzField(N * N);
        ifft2D(H, hField);
        ifft2D(Dx, dxField);
        ifft2D(Dz, dzField);

        // Assemble vertices: real parts of the three inverse transforms are
        // the height and the two horizontal displacement fields. The choppiness
        // parameter scales the horizontal displacement (0 => pure vertical).
        output.assign(N * N, FftOceanVertex{});
        const float cell = L / static_cast<float>(n);
        const float chop = config_.choppiness;
        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t x = 0; x < n; ++x) {
                const std::size_t idx = static_cast<std::size_t>(z) * N + x;
                const float hx = hField[idx].real();
                const float dispX = dxField[idx].real();
                const float dispZ = dzField[idx].real();
                const float px = static_cast<float>(x) * cell;
                const float pz = static_cast<float>(z) * cell;
                FftOceanVertex v;
                v.grid = glm::vec2(px, pz);
                v.height = hx;
                v.position = glm::vec3(px + dispX * chop,
                                       hx,
                                       pz + dispZ * chop);
                // Approx normal from the (small) analytic gradient estimate:
                // finite-difference slope from the neighboring cell heights.
                const float scale = 0.06f;
                float slopeX = 0.0f, slopeZ = 0.0f;
                if (x + 1 < n) {
                    const auto& e = hField[static_cast<std::size_t>(z) * N + (x + 1)];
                    slopeX = (e.real() - hx) / (cell * 1.0f);
                }
                if (z + 1 < n) {
                    const auto& e = hField[static_cast<std::size_t>(z + 1) * N + x];
                    slopeZ = (e.real() - hx) / (cell * 1.0f);
                }
                v.normal = glm::normalize(glm::vec3(-slopeX * scale, 1.0f, -slopeZ * scale));
                if (!std::isfinite(v.normal.x) || !std::isfinite(v.normal.z))
                    v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                output[idx] = v;
            }
        }
        return true;
    }

private:
    FftOceanConfig config_;
    std::unique_ptr<IFftCore> fft_;
    std::string err_;
};

}  // namespace

// Out-of-line member (header declared a member function body for the struct).
bool FftOceanConfig::valid(std::string& errorOut) const {
    if (size < 16 || size > 1024 || (size & (size - 1)) != 0) {
        errorOut = "size must be a power of two in [16, 1024]";
        return false;
    }
    if (!(tileSizeMeters >= 16.0f && tileSizeMeters <= 8192.0f)) {
        errorOut = "tileSizeMeters must be in [16, 8192]";
        return false;
    }
    if (!(windSpeed >= 0.5f && windSpeed <= 40.0f)) {
        errorOut = "windSpeed must be in [0.5, 40]";
        return false;
    }
    if (!std::isfinite(windDirRad)) {
        errorOut = "windDirRad must be finite";
        return false;
    }
    if (!(choppiness >= 0.0f && choppiness <= 4.0f)) {
        errorOut = "choppiness must be in [0, 4]";
        return false;
    }
    if (!(amplitude >= 0.01f && amplitude <= 8.0f)) {
        errorOut = "amplitude must be in [0.01, 8]";
        return false;
    }
    if (seed == 0) {
        errorOut = "seed must be non-zero";
        return false;
    }
    return true;
}

std::unique_ptr<IFftOceanSurface> create_fft_ocean_surface(std::string& errorOut) {
    auto impl = std::make_unique<FftOceanSurface>();
    if (!impl) { errorOut = "FftOceanSurface: allocation failed"; return nullptr; }
    return impl;
}

std::unique_ptr<IFftOceanSurface> create_fft_ocean_surface_json(
    const std::string& jsonText, std::string& errorOut) {
    auto impl = std::make_unique<FftOceanSurface>();
    if (!impl->configure_json(jsonText, errorOut)) return nullptr;
    return impl;
}

}  // namespace Engine::Rendering