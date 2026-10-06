// The one translation unit that compiles miniaudio, with stb_vorbis for Ogg Vorbis. miniaudio decodes
// Vorbis only when stb_vorbis's declarations come before its implementation, and stb_vorbis's own
// implementation after it (miniaudio's documented order).
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
