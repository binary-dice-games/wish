// MIT License © 2026 Binary Dice Games
/// @file audio_output.cpp
/// @brief Implementation of wish::media::audio_output on miniaudio.
#include <media/audio_decoder.hpp>
#include <media/audio_output.hpp>
#include <media/miniaudio_config.hpp>

#include <algorithm>
#include <mutex>
#include <utility>

namespace bdg::wish::media {

namespace {

// One miniaudio context for the process: probing the audio backends is slow
// and, on a machine without sound, logs errors (ALSA) -- do it once, not
// for every player. Null when there is no real backend.
ma_context* shared_context() {
  static ma_context context;
  static ma_context* ready = nullptr;
  static std::once_flag once;
  std::call_once(once, [] {
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS)
      return;
    if (context.backend == ma_backend_null) {
      ma_context_uninit(&context); // a fake device: report "no sound card"
      return;
    }
    ready = &context; // kept for the process lifetime
  });
  return ready;
}

} // namespace

struct audio_output::impl {
  ma_device device{};
  pull_fn pull;
  bool initialized = false;

  static void on_data(ma_device* dev, void* out, const void* /*in*/, ma_uint32 frames) {
    auto* self = static_cast<impl*>(dev->pUserData);
    auto* samples = static_cast<float*>(out);
    size_t written = 0;
    try {
      written = std::min<size_t>(self->pull(samples, frames), frames);
    } catch (...) {
      written = 0; // never let an exception reach the audio thread
    }
    std::fill(samples + written * kAudioChannels, samples + static_cast<size_t>(frames) * kAudioChannels, 0.0f);
  }
};

audio_output::audio_output(pull_fn pull) : impl_(std::make_unique<impl>()) {
  impl_->pull = std::move(pull);
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = kAudioChannels;
  config.sampleRate = kAudioRate;
  config.dataCallback = &impl::on_data;
  config.pUserData = impl_.get();
  ma_context* context = shared_context();
  if (!context || ma_device_init(context, &config, &impl_->device) != MA_SUCCESS)
    return;
  impl_->initialized = true;
  ok_ = ma_device_start(&impl_->device) == MA_SUCCESS;
}

audio_output::~audio_output() {
  if (impl_->initialized)
    ma_device_uninit(&impl_->device); // stops the device and joins its thread
}

} // namespace bdg::wish::media
