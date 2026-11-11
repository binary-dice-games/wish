// MIT License © 2026 Binary Dice Games
/// @file audio_decoder.hpp
/// @brief miniaudio-backed audio file decoder used by wish's media playback
///        (see `media::playback`).
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

namespace bdg::wish::media {

/// @brief Sample rate every decoded audio chunk is resampled to.
inline constexpr int kAudioRate = 48000;

/// @brief Channel count every decoded audio chunk is mixed to (stereo).
inline constexpr int kAudioChannels = 2;

/// @brief Frames `audio_decoder::decode()` produces per call (at most).
inline constexpr size_t kAudioChunkFrames = 4096;

/// @brief What an audio file contains, known once it is opened.
struct media_info {
  double duration = 0.0; ///< Seconds; 0 when the format doesn't say.
};

/// @brief One decoded audio chunk: interleaved float32 stereo at kAudioRate.
struct audio_chunk {
  double pts = 0.0; ///< Presentation time of the first sample, in seconds.
  std::vector<float> samples;
};

/**
 * @brief Decodes one audio file, chunk by chunk, to float32 stereo at
 *        `kAudioRate`. Not thread-safe: one thread drives it (`playback`'s
 *        decode thread).
 *
 * Formats: WAV, FLAC, MP3 and Ogg Vorbis, all decoded by miniaudio inside
 * the server (no system library). The decoder reads only the one file it
 * was given -- none of these formats references other files -- and the
 * caller validated its path against the session sandbox.
 */
class audio_decoder {
 public:
  /**
   * @brief Opens @p file.
   * @throws std::runtime_error when the file can't be read or is not an
   *         audio file in a supported format.
   */
  explicit audio_decoder(const std::filesystem::path& file);
  ~audio_decoder();

  audio_decoder(const audio_decoder&) = delete;
  audio_decoder& operator=(const audio_decoder&) = delete;

  /// @brief Duration of the opened file.
  const media_info& info() const;

  /**
   * @brief Decodes the next chunk of up to `kAudioChunkFrames` frames.
   * @return False once the file is fully decoded (@p out is then empty).
   * @throws std::runtime_error on a decoding error.
   */
  bool decode(audio_chunk& out);

  /// @brief Moves to @p seconds (sample-accurate); the next `decode()`
  ///        starts there.
  /// @throws std::runtime_error when the file can't seek.
  void seek(double seconds);

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::wish::media
