// MIT License © 2026 Binary Dice Games
/// @file media_decoder.hpp
/// @brief FFmpeg-backed audio/video file decoder used by wish's media
///        playback (see `media::playback`).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace bdg::wish::media {

/// @brief Sample rate every decoded audio chunk is resampled to.
inline constexpr int kAudioRate = 48000;

/// @brief Channel count every decoded audio chunk is mixed to (stereo).
inline constexpr int kAudioChannels = 2;

/// @brief Largest video frame accepted, per side, in pixels.
inline constexpr int kMaxVideoSide = 4096;

/// @brief What a media file contains, known once it is opened.
struct media_info {
  double duration = 0.0; ///< Seconds; 0 when the container doesn't say.
  int width = 0;         ///< Video width in pixels; 0 without video.
  int height = 0;        ///< Video height in pixels; 0 without video.
  bool has_video = false;
  bool has_audio = false;
};

/// @brief One decoded video frame, converted to RGBA8 (pitch = 4 * width).
struct video_frame {
  double pts = 0.0; ///< Presentation time in seconds from the start.
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
};

/// @brief One decoded audio chunk: interleaved float32 stereo at kAudioRate.
struct audio_chunk {
  double pts = 0.0; ///< Presentation time of the first sample, in seconds.
  std::vector<float> samples;
};

/**
 * @brief Decodes one media file, frame by frame. Not thread-safe: one
 *        thread drives it (`playback`'s decode thread).
 *
 * The file is read through a custom FFmpeg I/O context over a stream this
 * class opened itself, and any attempt by a demuxer to open another URL or
 * file (playlists, concat lists, references) is refused. So decoding a file
 * can never read outside that one file -- the caller validated its path
 * against the session sandbox.
 *
 * Embedded cover art (an MP3's picture) is ignored, not treated as video.
 */
class media_decoder {
 public:
  /// @brief What `decode()` produced.
  enum class result { video, audio, eof };

  /**
   * @brief Opens @p file and its best video and audio streams.
   * @throws std::runtime_error when the file can't be read, isn't a media
   *         file FFmpeg understands, has neither audio nor video, or has a
   *         video larger than kMaxVideoSide.
   */
  explicit media_decoder(const std::filesystem::path& file);
  ~media_decoder();

  media_decoder(const media_decoder&) = delete;
  media_decoder& operator=(const media_decoder&) = delete;

  /// @brief Streams, size and duration of the opened file.
  const media_info& info() const;

  /**
   * @brief Decodes until one video frame or audio chunk is ready.
   * @return `result::video` (filled @p video), `result::audio` (filled
   *         @p audio) or `result::eof` once the file is fully decoded.
   * @throws std::runtime_error on a decoding error.
   */
  result decode(video_frame& video, audio_chunk& audio);

  /// @brief Moves to the keyframe at or before @p seconds and drops any
  ///        buffered decoder state. Frames before @p seconds may still
  ///        follow; the caller skips them.
  void seek(double seconds);

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::wish::media
