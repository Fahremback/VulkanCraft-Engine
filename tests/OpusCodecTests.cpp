// OpusCodecTests — gate do contrato IAudioCodec (§8 opus, DEPENDENCY_POLICY):
// prova o codec REAL da libopus vendida (external/solutions/opus) atrás da
// superfície pública — round-trip encode→decode, determinismo bit-exact,
// silêncio compacto, recusas all-or-nothing na criação e no encode/decode.

#include "engine/audio/IAudioCodec.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<float> make_sine(int count, double freq, int sampleRate) {
    std::vector<float> out(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out[static_cast<std::size_t>(i)] =
            static_cast<float>(0.7 * std::sin(2.0 * 3.141592653589793 *
                                              freq * i / sampleRate));
    }
    return out;
}

// O Opus tem lookahead de 6.5 ms (312 amostras @ 48 kHz): a saída começa
// com um pre-roll de silêncio e o sinal útil chega atrasado. A correlação
// honesta compara a entrada contra a saída deslocada pelo atraso conhecido,
// restrita à região sobreposta após o pre-roll.
double delayed_cross_correlation(const std::vector<float>& a,
                                 const std::vector<float>& b, int lookahead) {
    const int n = static_cast<int>(a.size());
    if (n == 0 || a.size() != b.size() || lookahead <= 0 || lookahead >= n)
        return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int i = 0; i + lookahead < n; ++i) {
        const double x = a[static_cast<std::size_t>(i)];
        const double y = b[static_cast<std::size_t>(i + lookahead)];
        dot += x * y;
        na += x * x;
        nb += y * y;
    }
    if (na == 0.0 || nb == 0.0) return 0.0;
    return dot / std::sqrt(na * nb);
}

void test_basic_config() {
    std::string error;
    auto codec = engine::audio::create_opus_codec(
        { 48000, 1, 960, 32000 }, error);
    check(codec != nullptr, "create 48k mono 20ms");
    check(codec->sample_rate() == 48000 && codec->channels() == 1 &&
              codec->frame_samples() == 960 && codec->bitrate() == 32000,
          "configuração refletida");
}

void test_roundtrip() {
    std::string error;
    auto codec = engine::audio::create_opus_codec(
        { 48000, 1, 960, 32000 }, error);
    check(codec != nullptr, "create para roundtrip");

    const std::vector<float> pcm = make_sine(960, 440.0, 48000);
    std::vector<std::uint8_t> packet;
    check(codec->encode_frame(pcm.data(), packet, error),
          "encode_frame sine");
    check(!packet.empty(), "pacote não-vazio");

    std::vector<float> out;
    check(codec->decode_frame(packet.data(), packet.size(), out, error),
          "decode_frame");
    check(out.size() == 960, "saída com 960 amostras");
    const double corr =
        delayed_cross_correlation(pcm, out, 312);  // lookahead 6.5 ms @ 48k
    std::printf("    [diag] correlação (pós-lookahead) = %.4f, pacote = %zu bytes\n",
                corr, packet.size());
    check(corr > 0.9,
          "forma de onda preservada (correlação pós-lookahead > 0.9)");

    double inEnergy = 0.0, outEnergy = 0.0;
    for (int i = 0; i < 960; ++i) {
        inEnergy += static_cast<double>(pcm[static_cast<std::size_t>(i)]) *
                    pcm[static_cast<std::size_t>(i)];
        outEnergy += static_cast<double>(out[static_cast<std::size_t>(i)]) *
                     out[static_cast<std::size_t>(i)];
    }
    check(inEnergy > 0.0 && outEnergy > 0.0 &&
              outEnergy / inEnergy > 0.4 && outEnergy / inEnergy < 1.6,
          "energia preservada dentro de 0.4..1.6x");
}

void test_determinism() {
    std::string errorA, errorB;
    auto a = engine::audio::create_opus_codec({ 48000, 1, 960, 24000 }, errorA);
    auto b = engine::audio::create_opus_codec({ 48000, 1, 960, 24000 }, errorB);
    check(a != nullptr && b != nullptr, "dois codecs novos");

    const std::vector<float> pcm = make_sine(960, 330.0, 48000);
    std::vector<std::uint8_t> pa, pb;
    check(a->encode_frame(pcm.data(), pa, errorA), "encode A");
    check(b->encode_frame(pcm.data(), pb, errorB), "encode B");
    check(pa == pb, "pacotes bit-idênticos entre instâncias (determinismo)");
}

void test_silence_compact() {
    std::string error;
    auto codec = engine::audio::create_opus_codec({ 48000, 1, 960, 16000 }, error);
    check(codec != nullptr, "create para silêncio");

    std::vector<float> silence(960, 0.0f);
    std::vector<std::uint8_t> packet;
    check(codec->encode_frame(silence.data(), packet, error),
          "encode silêncio");
    // Silêncio real comprime para pouquíssimos bytes (OPUS não emite quadro
    // cheio de LFE para entrada nula; o limite honesto é bem abaixo do pior
    // caso de 4000 bytes).
    check(packet.size() < 200, "silêncio comprime (< 200 bytes)");
}

void test_sequential_frames() {
    std::string error;
    auto codec = engine::audio::create_opus_codec({ 48000, 1, 960, 32000 }, error);
    check(codec != nullptr, "create para frames sequenciais");

    const std::vector<float> pcm = make_sine(960, 220.0, 48000);
    for (int frame = 0; frame < 3; ++frame) {
        std::vector<std::uint8_t> packet;
        check(codec->encode_frame(pcm.data(), packet, error),
              "encode frame sequencial");
        std::vector<float> out;
        check(codec->decode_frame(packet.data(), packet.size(), out, error),
              "decode frame sequencial");
        check(out.size() == 960, "tamanho de saída por frame");
    }
}

void test_rejections() {
    std::string error;
    check(engine::audio::create_opus_codec({ 12345, 1, 960, 32000 }, error) ==
              nullptr,
          "sampleRate inválido recusa");
    check(engine::audio::create_opus_codec({ 48000, 3, 960, 32000 }, error) ==
              nullptr,
          "channels 3 recusa");
    check(engine::audio::create_opus_codec({ 48000, 1, 777, 32000 }, error) ==
              nullptr,
          "frameSamples fora da grade recusa");
    check(engine::audio::create_opus_codec({ 48000, 1, 960, 10 }, error) ==
              nullptr,
          "bitrate 10 recusa");

    auto codec = engine::audio::create_opus_codec({ 48000, 1, 960, 32000 }, error);
    check(codec != nullptr, "create para recusas de frame");
    std::vector<std::uint8_t> packet;
    check(!codec->encode_frame(nullptr, packet, error),
          "encode com pcm null recusa");

    std::vector<std::uint8_t> garbage;
    garbage.push_back(0xff);
    garbage.push_back(0x00);
    garbage.push_back(0x11);
    std::vector<float> out;
    check(!codec->decode_frame(garbage.data(), garbage.size(), out, error),
          "decode de pacote malformado recusa");
}

}  // namespace

int main() {
    test_basic_config();
    test_roundtrip();
    test_determinism();
    test_silence_compact();
    test_sequential_frames();
    test_rejections();

    if (failures == 0) {
        std::printf("opus_codec_tests: all checks passed\n");
        return 0;
    }
    std::printf("opus_codec_tests: %d failure(s)\n", failures);
    return 1;
}
