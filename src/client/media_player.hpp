// MIT License © 2026 Binary Dice Games
/// @file media_player.hpp
/// @brief Client-side handle to a media player playing on the server, and
///        the calls into the server's `__WishMedia` service shared by
///        `wish::client` and `wish::standalone`.
///
/// The client needs no media library: the server decodes and plays the file
/// (which must already be in the session sandbox, e.g. uploaded with
/// `upload_file()`), and this class only sends commands. See docs/media.md.
#pragma once

#include "src/bison/bison_object.hpp"
#include "src/rmi/client/proxy.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace bdg::wish {

/// @brief A media player's state; values match `WISH_MEDIA_STATE_*`.
enum class media_state : int32_t {
  stopped = 0, ///< At the start, not playing (after open or stop()).
  playing = 1,
  paused = 2,
  ended = 3, ///< Reached the end without looping.
  error = 4, ///< The file failed to decode; see `media_status::error`.
};

/// @brief Options for `client::open_media()`.
struct media_options {
  bool autoplay = false; ///< Start playing right away.
  bool loop = false;     ///< Restart from the beginning at the end.
  float volume = 1.0f;   ///< 0 (mute) .. 1 (full).
};

/// @brief Snapshot of a player, from `media_player::status()`.
struct media_status {
  media_state state = media_state::stopped;
  double position = 0.0; ///< Seconds.
  double duration = 0.0; ///< Seconds; 0 when the file doesn't say.
  int32_t width = 0;     ///< Video size in pixels; 0 without video.
  int32_t height = 0;
  bool has_video = false;
  bool has_audio = false;
  float volume = 1.0f;
  bool loop = false;
  bool audio_output = false; ///< False: the server has no sound card (silent).
  std::string error;         ///< Why the player is in `media_state::error`.
};

/**
 * @brief A media file opened on the server by `client::open_media()`.
 *
 * Copies share one player. Every call is asynchronous: it returns a future
 * that throws through `get()` when the player was closed or the session
 * ended. Show its video with a `Video` widget whose `player` field is
 * `name()`, or with `Image` and `src = "media://" + name()`.
 *
 * Event handlers run on the client's event thread.
 */
class media_player {
 public:
  using proxy_ptr = std::shared_ptr<bison::rmi::proxy::dynamic>;

  /// @brief An invalid player (`valid()` is false).
  media_player() = default;

  /// @brief Wraps the proxies `client::open_media()` built.
  media_player(std::string name, proxy_ptr player, proxy_ptr service)
      : state_(std::make_shared<shared>(std::move(name), std::move(player), std::move(service))) {}

  /// @brief True for a player returned by `open_media()` and not closed.
  bool valid() const noexcept {
    return state_ && state_->player && !state_->closed.load();
  }

  /// @brief The name given to `open_media()`; widgets refer to it.
  const std::string& name() const {
    return require().name;
  }

  /// @brief Remote object id of the player (for generic proxy code).
  bison::key_t object_id() const {
    return require().player->object_id();
  }

  /// @brief Start or resume; from the end, restart from the beginning.
  std::future<void> play() {
    return command("play", {});
  }
  /// @brief Pause at the current position.
  std::future<void> pause() {
    return command("pause", {});
  }
  /// @brief Pause and rewind to the start.
  std::future<void> stop() {
    return command("stop", {});
  }
  /// @brief Move to @p seconds (clamped to the file); keeps playing/paused.
  std::future<void> seek(double seconds) {
    bison::dynamic p;
    p[bison::key_t{"seconds"}] = static_cast<float>(seconds);
    return command("seek", std::move(p));
  }
  /// @brief Set the volume, 0 (mute) .. 1.
  std::future<void> set_volume(float volume) {
    bison::dynamic p;
    p[bison::key_t{"volume"}] = volume;
    return command("set_volume", std::move(p));
  }
  /// @brief Whether reaching the end restarts from the beginning.
  std::future<void> set_loop(bool loop) {
    bison::dynamic p;
    p[bison::key_t{"loop"}] = loop;
    return command("set_loop", std::move(p));
  }

  /// @brief Current state, position and stream info.
  std::future<media_status> status() {
    auto s = state_;
    return std::async(std::launch::async, [s] {
      check(s);
      return parse_status(s->player->call(bison::key_t{"status"}, bison::dynamic{}).get());
    });
  }

  /// @brief Calls @p handler with the new state on every change.
  void on_state_changed(std::function<void(media_state)> handler) {
    require().player->onEvent(bison::key_t{"state_changed"}, [h = std::move(handler)](bison::dynamic p) {
      h(static_cast<media_state>(p.get_as<int32_t>(bison::key_t{"state"}, 0)));
    });
  }

  /// @brief Calls @p handler when playback reaches the end without looping.
  void on_ended(std::function<void()> handler) {
    require().player->onEvent(bison::key_t{"ended"}, [h = std::move(handler)](bison::dynamic) { h(); });
  }

  /// @brief Calls @p handler with the message when decoding fails.
  void on_error(std::function<void(const std::string&)> handler) {
    require().player->onEvent(bison::key_t{"error"}, [h = std::move(handler)](bison::dynamic p) {
      h(p.get_as<std::string>(bison::key_t{"message"}, ""));
    });
  }

  /**
   * @brief Stops the player and frees it on the server (decoder, audio
   *        device, texture). Every copy becomes unusable.
   */
  std::future<void> close() {
    auto s = state_;
    return std::async(std::launch::async, [s] {
      if (!s || !s->player || s->closed.exchange(true))
        return; // already closed
      bison::dynamic p;
      p[bison::key_t{"name"}] = s->name;
      if (s->service)
        s->service->call(bison::key_t{"close"}, std::move(p)).get();
      s->player->destroy(); // unregisters handlers, drops the server object
    });
  }

  /// @brief Parses the `status` RMI result (see media::media_player).
  static media_status parse_status(const bison::dynamic& r) {
    using bison::key_t;
    media_status st;
    st.state = static_cast<media_state>(r.get_as<int32_t>(key_t{"state"}, 0));
    st.position = r.get_as<float>(key_t{"position"}, 0.0f);
    st.duration = r.get_as<float>(key_t{"duration"}, 0.0f);
    st.width = r.get_as<int32_t>(key_t{"width"}, 0);
    st.height = r.get_as<int32_t>(key_t{"height"}, 0);
    st.has_video = r.get_as<bool>(key_t{"has_video"}, false);
    st.has_audio = r.get_as<bool>(key_t{"has_audio"}, false);
    st.volume = r.get_as<float>(key_t{"volume"}, 1.0f);
    st.loop = r.get_as<bool>(key_t{"loop"}, false);
    st.audio_output = r.get_as<bool>(key_t{"audio_output"}, false);
    st.error = r.get_as<std::string>(key_t{"error"}, "");
    return st;
  }

 private:
  struct shared {
    shared(std::string n, proxy_ptr p, proxy_ptr s) : name(std::move(n)), player(std::move(p)), service(std::move(s)) {}
    std::string name;
    proxy_ptr player;
    proxy_ptr service;
    std::atomic<bool> closed{false}; // a destroyed proxy must not be called
  };

  static void check(const std::shared_ptr<shared>& s) {
    if (!s || !s->player)
      throw std::logic_error("wish: invalid media player");
    if (s->closed.load())
      throw std::logic_error("wish: media player closed");
  }

  const shared& require() const {
    check(state_);
    return *state_;
  }

  std::future<void> command(const char* method, bison::dynamic params) {
    auto s = state_;
    std::string m = method;
    return std::async(std::launch::async, [s, m, p = std::move(params)]() mutable {
      check(s);
      s->player->call(bison::key_t{m.c_str()}, std::move(p)).get();
    });
  }

  std::shared_ptr<shared> state_;
};

namespace detail {

/// @brief Throws `std::logic_error` when the server plays no media (built
///        without FFmpeg, or an older server); see `has_media()`.
inline void require_media(const media_player::proxy_ptr& service) {
  if (!service)
    throw std::logic_error("wish: this server plays no media (see has_media())");
}

/**
 * @brief Synchronous `open` on the `__WishMedia` service.
 * @param make_proxy  Turns the returned object id into a proxy (the
 *                    client's or standalone's `make_proxy`).
 */
template <class MakeProxy>
media_player open_media_sync(const media_player::proxy_ptr& service, MakeProxy&& make_proxy, const std::string& name,
    const std::string& path, const media_options& options) {
  require_media(service);
  using bison::key_t;
  bison::dynamic args;
  args[key_t{"name"}] = name;
  args[key_t{"src"}] = path;
  args[key_t{"autoplay"}] = options.autoplay;
  args[key_t{"loop"}] = options.loop;
  args[key_t{"volume"}] = options.volume;
  auto result = service->call(key_t{"open"}, std::move(args)).get();
  const auto id = result.get_as<key_t>(key_t{"id"}, key_t{});
  if (id.id == 0)
    throw std::runtime_error("wish: the server returned no media player");
  auto player = std::make_shared<bison::rmi::proxy::dynamic>(make_proxy(id));
  return media_player{name, std::move(player), service};
}

} // namespace detail

} // namespace bdg::wish
