// Single translation unit that instantiates the miniaudio implementation.
// All other engine audio sources include <miniaudio.h> without defining the
// implementation macro, avoiding duplicate-symbol link errors.
//
// Vorbis support: miniaudio only compiles its Ogg Vorbis backend when
// stb_vorbis is included BEFORE miniaudio.h (it sets
// STB_VORBIS_INCLUDE_STB_VORBIS_H, which defines MA_HAS_VORBIS). Without this,
// ma_decoder_init_file() reports "no backend" for OGG and every compressed
// audio asset fails to import/preview. The header is pulled in once for the
// declarations, then again (implementation) after miniaudio's implementation,
// exactly like miniaudio's own tools/audioconverter example.
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
