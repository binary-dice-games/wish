// MIT License © 2026 Binary Dice Games
/// @file audio_decoder.cpp
/// @brief Implementation of wish::media::audio_decoder on miniaudio.
#include <media/audio_decoder.hpp>
#include <media/miniaudio_config.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>

namespace bdg::wish::media {

namespace {

[[noreturn]] void fail(const std::string& what, ma_result err) {
  throw std::runtime_error("wish media: " + what + ": " + ma_result_description(err));
}

// Opens @p path with miniaudio's own file reader: the wide-path entry point
// where paths are wide (Windows: non-ASCII names), the UTF-8 one elsewhere.
template <class Char>
ma_result init_from_file(const Char* path, const ma_decoder_config* config, ma_decoder* decoder) {
  if constexpr (std::is_same_v<Char, wchar_t>)
    return ma_decoder_init_file_w(path, config, decoder);
  else
    return ma_decoder_init_file(path, config, decoder);
}

} // namespace

struct audio_decoder::impl {
  ma_decoder decoder{};
  bool initialized = false;
  media_info info;

  ~impl() {
    if (initialized)
      ma_decoder_uninit(&decoder);
  }
};

audio_decoder::audio_decoder(const std::filesystem::path& file) : impl_(std::make_unique<impl>()) {
  auto& d = *impl_;
  // miniaudio converts every format to the one the output device plays.
  const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, kAudioChannels, kAudioRate);
  // miniaudio's result codes don't tell "unknown format" from "unreadable"
  // usefully here (mostly "Unknown error"), so the message doesn't add one.
  if (init_from_file(file.c_str(), &config, &d.decoder) != MA_SUCCESS)
    throw std::runtime_error(
        "wish media: " + file.filename().string() + " is not a playable audio file (WAV, FLAC, MP3 or Ogg Vorbis)");
  d.initialized = true;

  ma_uint64 frames = 0;
  if (ma_decoder_get_length_in_pcm_frames(&d.decoder, &frames) == MA_SUCCESS)
    d.info.duration = static_cast<double>(frames) / kAudioRate;
}

audio_decoder::~audio_decoder() = default;

const media_info& audio_decoder::info() const {
  return impl_->info;
}

bool audio_decoder::decode(audio_chunk& out) {
  auto& d = *impl_;
  ma_uint64 cursor = 0;
  if (ma_decoder_get_cursor_in_pcm_frames(&d.decoder, &cursor) != MA_SUCCESS)
    cursor = 0;
  out.pts = static_cast<double>(cursor) / kAudioRate;
  out.samples.resize(kAudioChunkFrames * kAudioChannels);
  ma_uint64 read = 0;
  const ma_result err = ma_decoder_read_pcm_frames(&d.decoder, out.samples.data(), kAudioChunkFrames, &read);
  out.samples.resize(static_cast<size_t>(read) * kAudioChannels);
  if (err != MA_SUCCESS && err != MA_AT_END)
    fail("decoding failed", err);
  return read > 0;
}

void audio_decoder::seek(double seconds) {
  if (!(seconds > 0))
    seconds = 0;
  const auto frame = static_cast<ma_uint64>(seconds * kAudioRate);
  const ma_result err = ma_decoder_seek_to_pcm_frame(&impl_->decoder, frame);
  if (err != MA_SUCCESS)
    fail("seeking failed", err);
}

} // namespace bdg::wish::media
