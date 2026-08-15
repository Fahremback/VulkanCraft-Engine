// Single translation unit that instantiates the miniaudio implementation.
// All other engine audio sources include <miniaudio.h> without defining the
// implementation macro, avoiding duplicate-symbol link errors.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
