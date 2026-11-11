// MIT License © 2026 Binary Dice Games
/// @file miniaudio_config.hpp
/// @brief Includes miniaudio with the feature set wish's media playback uses.
///
/// Every translation unit that includes miniaudio goes through this header,
/// so all of them (and `miniaudio_impl.cpp`, which compiles the library)
/// agree on the `MA_NO_*` switches. Compiled in: the device API
/// (`audio_output`) and the decoders (`audio_decoder`: WAV, FLAC and MP3
/// built in, Ogg Vorbis through stb_vorbis). Left out: encoding, waveform
/// generation, the resource manager, the node graph and the engine.
#pragma once

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include <miniaudio.h>
