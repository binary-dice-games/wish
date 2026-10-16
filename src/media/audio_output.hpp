// MIT License © 2026 Binary Dice Games
/// @file audio_output.hpp
/// @brief One playback device on the server's sound card (miniaudio).
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace bdg::wish::media {

/**
 * @brief Plays interleaved float32 stereo at `kAudioRate` on the default
 *        output device, pulling samples from a callback.
 *
 * miniaudio loads the platform's audio API at runtime (WASAPI, Core Audio,
 * PulseAudio/ALSA/JACK), so the server has no build-time audio dependency.
 * On a machine without a usable device (a headless server, CI), `ok()` is
 * false and the callback is never called; `playback` then keeps time with
 * the wall clock instead.
 */
class audio_output {
 public:
  /**
   * @brief Fills @p out with up to @p frames stereo frames (2 floats each).
   * @return Frames written; the rest is silence. Runs on the audio thread:
   *         must not block for long.
   */
  using pull_fn = std::function<size_t(float* out, size_t frames)>;

  /// @brief Opens and starts the default output device.
  explicit audio_output(pull_fn pull);

  /// @brief Stops and closes the device; no callback runs after it returns.
  ~audio_output();

  audio_output(const audio_output&) = delete;
  audio_output& operator=(const audio_output&) = delete;

  /// @brief True when a device was opened and is running.
  bool ok() const noexcept {
    return ok_;
  }

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
  bool ok_ = false;
};

} // namespace bdg::wish::media
