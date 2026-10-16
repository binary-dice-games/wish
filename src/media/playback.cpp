// MIT License © 2026 Binary Dice Games
/// @file playback.cpp
/// @brief Implementation of wish::media::playback.
#include <media/audio_output.hpp>
#include <media/playback.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

namespace bdg::wish::media {

namespace {

// Assumed length of the last video frame, for the end-of-file position.
constexpr double kLastFrameDuration = 1.0 / 30.0;

// Audio this far behind the position is dropped even without a device
// pulling it, so a stalled sound card can't stall decoding with it.
constexpr double kStaleAudio = 0.5;

double steady_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

const char* to_string(play_state s) {
  switch (s) {
    case play_state::stopped:
      return "stopped";
    case play_state::playing:
      return "playing";
    case play_state::paused:
      return "paused";
    case play_state::ended:
      return "ended";
    case play_state::error:
      return "error";
  }
  return "stopped";
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

playback::playback(const std::filesystem::path& file, playback_options options)
    : options_(std::move(options)), decoder_(std::make_unique<media_decoder>(file)), info_(decoder_->info()) {
  if (!options_.clock)
    options_.clock = steady_seconds;
  {
    auto s = state_.wlock();
    s->loop = options_.loop;
    s->volume = std::clamp(options_.volume, 0.0f, 1.0f);
  }
  if (info_.has_audio && options_.audio_device) {
    auto out = std::make_unique<audio_output>([this](float* dst, size_t frames) { return pull_audio(dst, frames); });
    if (out->ok())
      audio_out_ = std::move(out);
  }
  thread_ = std::thread([this] { decode_loop(); });
}

playback::~playback() {
  {
    auto s = state_.wlock();
    s->quit = true;
  }
  state_.notify_all();
  if (thread_.joinable())
    thread_.join();
  audio_out_.reset(); // no audio callback runs after this
}

// ── Controls ─────────────────────────────────────────────────────────────────

void playback::play() {
  {
    auto s = state_.wlock();
    if (s->state == play_state::playing || s->state == play_state::error)
      return;
    if (s->state == play_state::ended)
      seek_locked(*s, 0.0);
    s->wall_anchor = now();
    set_state_locked(*s, play_state::playing);
  }
  state_.notify_all();
}

void playback::pause() {
  auto s = state_.wlock();
  if (s->state != play_state::playing)
    return;
  s->base = position_locked(*s);
  set_state_locked(*s, play_state::paused);
}

void playback::stop() {
  {
    auto s = state_.wlock();
    if (s->state == play_state::error)
      return;
    seek_locked(*s, 0.0);
    set_state_locked(*s, play_state::stopped);
  }
  state_.notify_all();
}

void playback::seek(double seconds) {
  {
    auto s = state_.wlock();
    if (s->state == play_state::error)
      return;
    seek_locked(*s, seconds);
    if (s->state == play_state::ended)
      set_state_locked(*s, play_state::paused);
  }
  state_.notify_all();
}

void playback::set_volume(float volume) {
  state_.wlock()->volume = std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 1.0f;
}

void playback::set_loop(bool loop) {
  state_.wlock()->loop = loop;
}

play_state playback::state() const {
  return state_.rlock()->state;
}

double playback::position() const {
  return position_locked(*state_.rlock());
}

float playback::volume() const {
  return state_.rlock()->volume;
}

bool playback::loop() const {
  return state_.rlock()->loop;
}

std::string playback::error() const {
  return state_.rlock()->error;
}

// ── Render-loop side ─────────────────────────────────────────────────────────

std::vector<playback_event> playback::update() {
  std::vector<playback_event> events;
  bool wake_decoder = false;
  {
    auto s = state_.wlock();
    const double pos = position_locked(*s);

    if (s->state == play_state::playing) {
      // Newest frame whose time has come; older due frames are skipped.
      while (!s->frames.empty() && s->frames.front().pts <= pos + 1e-3) {
        s->current = std::move(s->frames.front());
        s->frames.pop_front();
        s->preview_shown = true;
        wake_decoder = true;
      }
    } else if (!s->preview_shown && !s->frames.empty()) {
      // Not playing: show the frame at the position once (after open or a
      // seek); it stays queued and becomes due again once playing.
      s->current = s->frames.front();
      s->preview_shown = true;
    }

    if (!audio_out_ || s->state != play_state::playing) {
      // Nobody consumes audio: drop what's stale so decoding never stalls.
      while (s->samples.size() >= kAudioChannels && s->samples_pts < pos - kStaleAudio) {
        const size_t n = std::min(s->samples.size(), static_cast<size_t>(kAudioRate / 10) * kAudioChannels);
        s->samples.erase(s->samples.begin(), s->samples.begin() + static_cast<std::ptrdiff_t>(n));
        s->samples_pts += static_cast<double>(n / kAudioChannels) / kAudioRate;
        wake_decoder = true;
      }
    }

    if (s->state == play_state::playing && s->eof && s->frames.empty() && pos >= s->end_pts) {
      if (s->loop) {
        seek_locked(*s, 0.0);
        s->wall_anchor = now();
        wake_decoder = true;
      } else {
        s->base = s->end_pts;
        s->samples.clear();
        set_state_locked(*s, play_state::ended);
        s->events.push_back({playback_event::kind::ended, play_state::ended, {}});
      }
    }
    events.swap(s->events);
  }
  if (wake_decoder)
    state_.notify_all();
  return events;
}

bool playback::take_frame(video_frame& out) {
  auto s = state_.wlock();
  if (!s->current)
    return false;
  out = std::move(*s->current);
  s->current.reset();
  return true;
}

bool playback::wait_buffered(double timeout_s) {
  return state_.wait_for(std::chrono::duration<double>(timeout_s),
      [](const shared_state& s) { return !s.frames.empty() || !s.samples.empty() || s.eof; });
}

// ── Internals (state_ locked by the caller) ──────────────────────────────────

double playback::now() const {
  return options_.clock();
}

double playback::position_locked(const shared_state& s) const {
  if (s.state != play_state::playing)
    return s.base;
  return s.base + std::max(0.0, now() - s.wall_anchor);
}

void playback::set_state_locked(shared_state& s, play_state st) {
  if (s.state == st)
    return;
  s.state = st;
  s.events.push_back({playback_event::kind::state_changed, st, {}});
}

void playback::seek_locked(shared_state& s, double seconds) {
  if (!std::isfinite(seconds) || seconds < 0)
    seconds = 0;
  if (info_.duration > 0 && seconds > info_.duration)
    seconds = info_.duration;
  s.base = seconds;
  s.wall_anchor = now();
  s.seek_to = seconds;
  s.frames.clear();
  s.samples.clear();
  s.current.reset();
  s.preview_shown = false;
  s.eof = false;
  s.end_pts = seconds;
}

bool playback::buffers_full(const shared_state& s) const {
  if (s.frames.size() >= kMaxQueuedFrames)
    return true;
  return audio_out_ && s.samples.size() >= kMaxQueuedSamples;
}

size_t playback::pull_audio(float* out, size_t frames) {
  size_t written = 0;
  {
    auto s = state_.wlock();
    if (s->state != play_state::playing)
      return 0;
    const double pos = position_locked(*s);
    if (s->samples.empty())
      return 0;

    const double ahead = s->samples_pts - pos;
    if (ahead > kAudioSyncTolerance) {
      // Audio is early (e.g. right after a seek into a gap): pad.
      written = std::min(frames, static_cast<size_t>(ahead * kAudioRate));
      std::fill(out, out + written * kAudioChannels, 0.0f);
    } else if (ahead < -kAudioSyncTolerance) {
      // Audio is late (the device drifted or stalled): catch up.
      const size_t late = std::min(s->samples.size() / kAudioChannels, static_cast<size_t>(-ahead * kAudioRate));
      s->samples.erase(s->samples.begin(), s->samples.begin() + static_cast<std::ptrdiff_t>(late * kAudioChannels));
      s->samples_pts += static_cast<double>(late) / kAudioRate;
    }

    const size_t n = std::min(frames - written, s->samples.size() / kAudioChannels);
    const float vol = s->volume;
    float* dst = out + written * kAudioChannels;
    for (size_t i = 0; i < n * kAudioChannels; ++i)
      dst[i] = s->samples[i] * vol;
    s->samples.erase(s->samples.begin(), s->samples.begin() + static_cast<std::ptrdiff_t>(n * kAudioChannels));
    s->samples_pts += static_cast<double>(n) / kAudioRate;
    written += n;
  }
  state_.notify_all(); // room for more audio
  return written;
}

// ── Decode thread ────────────────────────────────────────────────────────────

void playback::decode_loop() {
  video_frame video;
  audio_chunk audio;
  for (;;) {
    std::optional<double> seek_target;
    state_.wait([this](const shared_state& s) { return s.quit || s.seek_to || (!s.eof && !buffers_full(s)); });
    {
      auto s = state_.wlock();
      if (s->quit)
        return;
      if (s->seek_to) {
        seek_target = s->seek_to;
        s->seek_to.reset();
      } else if (s->eof || buffers_full(*s)) {
        continue; // woke for a change that was already undone
      }
    }

    bool notify = false;
    try {
      if (seek_target) {
        decoder_->seek(*seek_target);
        auto s = state_.wlock();
        if (!s->seek_to) {
          s->skip_until = *seek_target;
          s->frames.clear();
          s->samples.clear();
        }
        continue;
      }

      const auto r = decoder_->decode(video, audio);
      auto s = state_.wlock();
      if (s->seek_to || s->quit)
        continue; // stale: the pending seek is handled next
      switch (r) {
        case media_decoder::result::video:
          s->end_pts = std::max(s->end_pts, video.pts + kLastFrameDuration);
          if (video.pts + 1e-3 < s->skip_until && !s->frames.empty() && s->frames.back().pts + 1e-3 < s->skip_until) {
            // Seeking between keyframe and target: keep only the newest
            // frame before the target, which is what shows at the target.
            s->frames.back() = std::move(video);
          } else {
            s->frames.push_back(std::move(video));
          }
          notify = true;
          break;
        case media_decoder::result::audio: {
          const size_t chunk_frames = audio.samples.size() / kAudioChannels;
          const double dur = static_cast<double>(chunk_frames) / kAudioRate;
          s->end_pts = std::max(s->end_pts, audio.pts + dur);
          if (!audio_out_ || audio.pts + dur <= s->skip_until)
            break; // silent player, or before the seek target
          size_t skip = 0;
          if (audio.pts < s->skip_until)
            skip = std::min(chunk_frames, static_cast<size_t>((s->skip_until - audio.pts) * kAudioRate));
          if (s->samples.empty())
            s->samples_pts = audio.pts + static_cast<double>(skip) / kAudioRate;
          s->samples.insert(s->samples.end(), audio.samples.begin() + static_cast<std::ptrdiff_t>(skip * kAudioChannels),
              audio.samples.end());
          break;
        }
        case media_decoder::result::eof:
          s->eof = true;
          notify = true;
          break;
      }
    } catch (const std::exception& e) {
      auto s = state_.wlock();
      s->error = e.what();
      s->eof = true;
      s->frames.clear();
      s->samples.clear();
      set_state_locked(*s, play_state::error);
      s->events.push_back({playback_event::kind::error, play_state::error, e.what()});
      notify = true;
    }
    state_.notify_all(); // wait_buffered()
    if (notify && options_.wake)
      options_.wake();
  }
}

} // namespace bdg::wish::media
