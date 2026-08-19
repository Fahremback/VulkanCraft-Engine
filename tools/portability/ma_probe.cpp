#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#include <cstdio>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
        ma_decoder dec;
        ma_result r = ma_decoder_init_file(argv[i], &cfg, &dec);
        printf("%s -> %d (%s)\n", argv[i], (int)r, ma_result_description(r));
        if (r == MA_SUCCESS) {
            ma_uint64 frames = 0;
            ma_decoder_get_length_in_pcm_frames(&dec, &frames);
            printf("  channels=%u rate=%u frames=%llu\n",
                   (unsigned)dec.outputChannels, (unsigned)dec.outputSampleRate,
                   (unsigned long long)frames);
            ma_decoder_uninit(&dec);
        }
    }
    return 0;
}
