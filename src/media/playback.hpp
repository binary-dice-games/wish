// MIT License © 2026 Binary Dice Games
/// @file playback.hpp
/// @brief Plays one media file: decode thread, audio output, A/V clock and
///        the play/pause/seek state machine behind a `media_player`.
#pragma once

#include <media/media_decoder.hpp>

#include "src/bison/bison_sync.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bdg::wish::media {

class audio_output;

/// @brief Where a player is in its lifecycle. The numeric values are part
///        of the RMI and C ABI contract (`WISH_MEDIA_STATE_*`).
enum class play_state : int32_t {
  stopped = 0, ///< At the start, not playing (after open or stop()).
  playing = 1,
  paused = 2,
  ended = 3, ///< Reached the end without looping.
  error = 4, ///< Decoding failed; see `playback::error()`.
};

/// @brief Name used for @p s in RMI payloads and logs ("playing", ...).
const char* to_string(play_state s);

/// @brief Something a client should hear about, collected by `update()`.
struct playback_event {
  enum class kind { state_changed, ended, error };
  kind what;
  play_state state;
  std::string message; ///< Error text for `kind::error`.
};

/// @brief Options for one `playback`.
struct playback_options {
  bool loop = false;
  float volume = 1.0f;
  /// Open the sound card. False keeps time with `clock` only (tests, or a
  /// server that must stay silent).
  bool audio_device = true;
  /// Monotonic seconds; defaults to std::chrono::steady_clock. Drives the
  /// position whenever no audio device does (tests inject a manual clock).
  std::function<double()> clock;
  /// Called from the decode thread when something changed that `update()`
  /// should pick up (a frame is ready, the end was reached, an error).
  std::function<void()> wake;
};

/**
 * @brief Plays one opened media file.
 *
 * A decode thread keeps a few RGBA video frames and up to a second of audio
 * buffered ahead of the play position, also while paused, so the first
 * frame shows right after open or seek.
 *
 * The position follows `playback_options::clock` (the wall clock) while
 * playing. Video frames are shown when their time comes; audio goes to an
 * `audio_output`, which drops late samples or pads with silence whenever the
 * sound card drifts more than `kAudioSyncTolerance` from that position. A
 * sound-card clock would follow the device more exactly, but it stalls
 * whenever audio runs out (a short audio track, coarse interleaving), which
 * would freeze the video with it.
 *
 * Every public method is thread-safe. `update()` and `take_frame()` are
 * meant for one thread, the render loop, which also owns the texture the
 * frames go to.
 */
class playback {
 public:
  static constexpr size_t kMaxQueuedFrames = 4;
  static constexpr size_t kMaxQueuedSamples = kAudioRate * kAudioChannels; // 1 s
  static constexpr double kAudioSyncTolerance = 0.05; // seconds

  /**
   * @brief Opens @p file (an already sandbox-validated path).
   * @throws std::runtime_error when it is not a playable media file.
   */
  playback(const std::filesystem::path& file, playback_options options);

  /// @brief Stops the decode thread and closes the audio device.
  ~playback();

  playback(const playback&) = delete;
  playback& operator=(const playback&) = delete;

  /// @brief Streams, size and duration of the file.
  const media_info& info() const noexcept {
    return info_;
  }

  /// @brief Starts or resumes; from `ended`, restarts from the beginning.
  void play();
  /// @brief Pauses at the current position (no-op unless playing).
  void pause();
  /// @brief Pauses and rewinds to the start.
  void stop();
  /// @brief Moves to @p seconds (clamped to [0, duration]), keeping the
  ///        playing/paused state; `ended` becomes `paused`.
  void seek(double seconds);
  /// @brief Sets the volume, clamped to [0, 1].
  void set_volume(float volume);
  /// @brief Whether reaching the end restarts from the beginning.
  void set_loop(bool loop);

  play_state state() const;
  /// @brief Current position in seconds.
  double position() const;
  float volume() const;
  bool loop() const;
  /// @brief True when audio goes to a sound card (false: silent, wall clock).
  bool audio_output_active() const noexcept {
    return audio_out_ != nullptr;
  }
  /// @brief Message of the error that put the player in `play_state::error`.
  std::string error() const;

  /**
   * @brief Advances the player: picks the newest video frame that is due,
   *        detects the end (looping or stopping), and returns the events
   *        raised since the last call.
   */
  std::vector<playback_event> update();

  /// @brief Moves the newest due frame picked by `update()` into @p out.
  /// @return False when no new frame was picked since the last call.
  bool take_frame(video_frame& out);

  /// @brief Blocks until a frame or audio is queued (not yet picked by
  ///        `update()`), the file is fully decoded, or @p timeout_s passes
  ///        (tests).
  /// @return True unless the timeout passed.
  bool wait_buffered(double timeout_s);

 private:
  struct shared_state {
    play_state state = play_state::stopped;
    double base = 0.0;          // position at the last anchor
    double wall_anchor = 0.0;   // clock() when playing started
    std::deque<video_frame> frames;
    std::deque<float> samples;  // interleaved stereo, starting at samples_pts
    double samples_pts = 0.0;
    double skip_until = 0.0;    // drop decoded data before this time (seek)
    std::optional<double> seek_to;
    bool eof = false;
    double end_pts = 0.0;       // end of the last decoded frame/chunk
    bool quit = false;
    bool loop = false;
    float volume = 1.0f;
    std::optional<video_frame> current; // newest due frame not yet taken
    bool preview_shown = false;         // first frame shown while not playing
    std::vector<playback_event> events;
    std::string error;
  };

  double now() const;
  bool buffers_full(const shared_state& s) const;
  double position_locked(const shared_state& s) const;
  void set_state_locked(shared_state& s, play_state st);
  void seek_locked(shared_state& s, double seconds);
  void decode_loop();
  size_t pull_audio(float* out, size_t frames);

  playback_options options_;
  std::unique_ptr<media_decoder> decoder_;
  media_info info_;
  bison::synchronized<shared_state> state_;
  std::unique_ptr<audio_output> audio_out_;
  std::thread thread_;
};

} // namespace bdg::wish::media
