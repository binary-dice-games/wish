// MIT License © 2026 Binary Dice Games
/// @file miniaudio_impl.cpp
/// @brief The one translation unit holding miniaudio's and stb_vorbis'
///        implementations.
///
/// stb_vorbis' declarations come first so miniaudio enables its Ogg Vorbis
/// decoder; stb_vorbis' implementation must follow miniaudio's (see
/// miniaudio.h, "Vorbis").

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <media/miniaudio_config.hpp>

#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
