// opus-probe.cpp — verifies Opus codec is usable (encode + decode round-trip)
// Build: cl /MD /EHsc /I <opus-include-dir> opus-probe.cpp /link <opus.lib> winmm.lib ole32.lib
// The probe encodes a sine wave to Opus and decodes it back, verifying round-trip.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

// Opus C API
#include <opus.h>

static int checks_passed = 0;
static int checks_total = 0;

#define CHECK(expr, msg) do { \
    checks_total++; \
    if (expr) { checks_passed++; } \
    else { printf("  FAIL: %s\n", msg); } \
} while(0)

int main() {
    printf("=== opus-probe ===\n");

    // 1. Version check
    int ver = opus_get_version_string();
    printf("  Opus version: %s\n", opus_get_version_string());
    CHECK(ver != 0, "opus_get_version_string returned 0");

    // 2. Encoder creation (48kHz mono)
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    CHECK(enc != nullptr, "encoder creation failed");
    CHECK(err == OPUS_OK, "encoder creation error");

    if (enc) {
        // 3. Encoder config
        err = opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
        CHECK(err == OPUS_OK, "set bitrate failed");

        err = opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
        CHECK(err == OPUS_OK, "set complexity failed");

        // 4. Generate 20ms of sine wave (960 samples at 48kHz)
        const int frame_size = 960;
        std::vector<float> pcm(frame_size);
        for (int i = 0; i < frame_size; i++) {
            pcm[i] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * i / 48000.0f);
        }

        // 5. Encode
        std::vector<unsigned char> packet(4000);
        int nb_bytes = opus_encode(enc, pcm.data(), frame_size, packet.data(), (int)packet.size());
        CHECK(nb_bytes > 0, "encode returned 0 or negative bytes");
        CHECK(nb_bytes < (int)packet.size(), "encode output too large");

        printf("  Encoded %d samples -> %d bytes\n", frame_size, nb_bytes);

        // 6. Decoder creation
        OpusDecoder* dec = opus_decoder_create(48000, 1, &err);
        CHECK(dec != nullptr, "decoder creation failed");
        CHECK(err == OPUS_OK, "decoder creation error");

        if (dec) {
            // 7. Decode
            std::vector<float> decoded(frame_size);
            int decoded_samples = opus_decode(dec, packet.data(), nb_bytes, decoded.data(), frame_size, 0);
            CHECK(decoded_samples == frame_size, "decoded sample count mismatch");

            // 8. Round-trip quality (SNR check)
            float signal_energy = 0.0f;
            float noise_energy = 0.0f;
            for (int i = 0; i < frame_size; i++) {
                float diff = pcm[i] - decoded[i];
                signal_energy += pcm[i] * pcm[i];
                noise_energy += diff * diff;
            }
            float snr = (noise_energy > 1e-10f) ?
                10.0f * log10f(signal_energy / noise_energy) : 999.0f;
            printf("  Round-trip SNR: %.1f dB\n", snr);
            CHECK(snr > 10.0f, "SNR too low (decode quality bad)");

            opus_decoder_destroy(dec);
        }

        opus_encoder_destroy(enc);
    }

    printf("\n=== Results: %d/%d passed ===\n", checks_passed, checks_total);
    return checks_passed == checks_total ? 0 : 1;
}
