// MIT License © 2026 Binary Dice Games
/**
 * @file media.hpp
 * @brief RAII wrapper around `wish_media_handle`: an audio/video player
 *        playing on the server (see `client::open_media()` and
 *        docs/media.md).
 */
#pragma once

#include "error.hpp"

#include <wish_client_c.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace bdg::wish::binding {

/// @brief A player's state (matches WISH_MEDIA_STATE_*).
enum class media_state : int32_t { stopped = 0, playing = 1, paused = 2, ended = 3, error = 4 };

/// @brief Options for `client::open_media()`.
struct media_options {
  bool autoplay = false; ///< Start playing right away.
  bool loop = false;     ///< Restart from the beginning at the end.
  float volume = 1.0f;   ///< 0 (mute) .. 1.
};

/// @brief Snapshot of a player (see wish_media_get_status()).
struct media_status {
  media_state state = media_state::stopped;
  double position = 0.0; ///< Seconds.
  double duration = 0.0; ///< Seconds; 0 when the file doesn't say.
  int32_t width = 0;
  int32_t height = 0;
  bool has_video = false;
  bool has_audio = false;
  float volume = 1.0f;
  bool loop = false;
  bool audio_output = false; ///< False: the server has no sound card.
};

/**
 * @brief A media file playing (or ready to play) on the server.
 *
 * Move-only; closing (explicitly or on destruction) stops the player and
 * frees it on the server. Show its video with a `Video` widget whose
 * `player` field is `name()`.
 */
class media_player {
 public:
  /// @brief Every event (see wish_media_event_fn); runs on the client's
  ///        event thread.
  struct event {
    enum class kind : int32_t { state_changed = 0, ended = 1, error = 2 } what;
    media_state state;
    std::string message; ///< Error text for `kind::error`.
  };
  using event_handler = std::function<void(const event&)>;

  media_player(wish_media_handle h, wish_client_handle client, std::string name)
      : h_(h), client_(client), name_(std::move(name)), handler_(std::make_unique<handler_slot>()) {}

  media_player(media_player&& other) noexcept
      : h_(std::exchange(other.h_, nullptr)),
        client_(other.client_),
        name_(std::move(other.name_)),
        handler_(std::move(other.handler_)) {}

  media_player& operator=(media_player&& other) noexcept {
    if (this != &other) {
      close();
      h_ = std::exchange(other.h_, nullptr);
      client_ = other.client_;
      name_ = std::move(other.name_);
      handler_ = std::move(other.handler_);
    }
    return *this;
  }

  media_player(const media_player&) = delete;
  media_player& operator=(const media_player&) = delete;

  ~media_player() {
    close();
  }

  /// @brief The name widgets refer to.
  const std::string& name() const noexcept {
    return name_;
  }

  /// @brief False once closed or moved from.
  bool valid() const noexcept {
    return h_ != nullptr;
  }

  void play() {
    check(wish_media_play(require()), "media_player::play");
  }
  void pause() {
    check(wish_media_pause(require()), "media_player::pause");
  }
  void stop() {
    check(wish_media_stop(require()), "media_player::stop");
  }
  void seek(double seconds) {
    check(wish_media_seek(require(), seconds), "media_player::seek");
  }
  void set_volume(float volume) {
    check(wish_media_set_volume(require(), volume), "media_player::set_volume");
  }
  void set_loop(bool loop) {
    check(wish_media_set_loop(require(), loop ? 1 : 0), "media_player::set_loop");
  }

  media_status status() {
    wish_media_status s{};
    check(wish_media_get_status(require(), &s), "media_player::status");
    media_status out;
    out.state = static_cast<media_state>(s.state);
    out.position = s.position;
    out.duration = s.duration;
    out.width = s.width;
    out.height = s.height;
    out.has_video = s.has_video != 0;
    out.has_audio = s.has_audio != 0;
    out.volume = s.volume;
    out.loop = s.loop != 0;
    out.audio_output = s.audio_output != 0;
    return out;
  }

  /// @brief Why the player is in `media_state::error` ("" otherwise).
  std::string error_message() {
    char* text = nullptr;
    check(wish_media_get_error(require(), &text), "media_player::error_message");
    std::string out = text ? text : "";
    bison_free_string(text);
    return out;
  }

  /// @brief Receive every event in @p handler (replacing an earlier one).
  void on_event(event_handler handler) {
    {
      std::lock_guard<std::mutex> lock(handler_->mutex);
      handler_->fn = std::move(handler);
    }
    check(wish_media_on_event(require(), &media_player::trampoline, handler_.get()), "media_player::on_event");
  }

  /// @brief Stop and free the player on the server; a no-op when closed.
  void close() noexcept {
    if (h_)
      wish_media_close(std::exchange(h_, nullptr)); // no callback runs after this
  }

 private:
  struct handler_slot {
    std::mutex mutex;
    event_handler fn;
  };

  static void trampoline(wish_media_handle, int32_t ev, int32_t state, const char* message, void* ud) {
    auto* slot = static_cast<handler_slot*>(ud);
    event_handler fn;
    {
      std::lock_guard<std::mutex> lock(slot->mutex);
      fn = slot->fn;
    }
    if (!fn)
      return;
    try {
      fn(event{static_cast<event::kind>(ev), static_cast<media_state>(state), message ? message : ""});
    } catch (...) {
      // never let an exception cross the C ABI
    }
  }

  wish_media_handle require() const {
    if (!h_)
      throw error(WISH_ERR_NULL, "media_player: closed");
    return h_;
  }

  void check(wish_error rc, const char* context) const {
    detail::throw_if_wish_error(rc, context, client_);
  }

  wish_media_handle h_;
  wish_client_handle client_;
  std::string name_;
  std::unique_ptr<handler_slot> handler_; // stable address for the C callback
};

} // namespace bdg::wish::binding
