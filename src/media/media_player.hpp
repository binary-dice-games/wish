// MIT License © 2026 Binary Dice Games
/// @file media_player.hpp
/// @brief One named audio/video player of a session, exposed over RMI.
#pragma once

#include <media/playback.hpp>
#include <server/renderer.hpp>

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_sync.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace bdg::wish {

struct context;

namespace media {

class media_player;
using media_player_ptr = std::shared_ptr<media_player>;

/**
 * @brief A media file playing (or ready to play) on the server, created by
 *        `media_service::open()` and reachable by the client as an RMI
 *        object of class `"__WishMediaPlayer"`.
 *
 * ## RMI methods
 *
 * | Method       | Params                   | Result |
 * |--------------|--------------------------|--------|
 * | `play`       | –                        | –      |
 * | `pause`      | –                        | –      |
 * | `stop`       | –                        | –      |
 * | `seek`       | `"seconds"`: float      | –      |
 * | `set_volume` | `"volume"`: float (0..1) | –      |
 * | `set_loop`   | `"loop"`: bool           | –      |
 * | `status`     | –                        | `"state"` int32 (`play_state`), `"state_name"`, `"position"`/`"duration"` float (seconds), `"width"`/`"height"` int32, `"has_video"`/`"has_audio"`/`"loop"`/`"audio_output"` bool, `"volume"` float, `"error"` string |
 *
 * ## Events (to the client's proxy of this object)
 *
 * - `state_changed` `{"state": int32, "state_name": string}`
 * - `ended` `{}` -- reached the end without looping.
 * - `error` `{"message": string}` -- decoding failed; the player stays in
 *   the `error` state.
 *
 * Calls after `close()` throw ("media player closed").
 */
class media_player : public bison::dynamic {
 public:
  /**
   * @brief Opens @p file (a sandbox-validated path) as player @p name.
   * @throws std::runtime_error when the file isn't playable.
   */
  static media_player_ptr instantiate(std::string name, const std::filesystem::path& file, playback_options options);

  media_player(bison::dynamic&& base, std::string name, std::unique_ptr<playback> pb);

  media_player(const media_player&) = delete;
  media_player& operator=(const media_player&) = delete;

  /// @brief The name widgets use to show this player (`Video.player`).
  const std::string& name() const noexcept {
    return name_;
  }

  /// @brief RMI object id the client knows this player by (0 until set).
  bison::key_t rmi_id() const noexcept {
    return rmi_id_;
  }
  void set_rmi_id(bison::key_t id) noexcept {
    rmi_id_ = id;
  }

  /// @brief The playback engine; null once closed.
  std::shared_ptr<playback> engine() const {
    return *pb_.rlock();
  }

  /// @brief The `status` RMI result (see class doc).
  bison::dynamic status() const;

  /**
   * @brief Advances playback and uploads the newest due frame to this
   *        player's texture; queues events for the client. Render thread,
   *        session locked.
   * @return True when something on screen or for the client changed.
   */
  bool pump(const context& s, renderer& r);

  /// @brief Texture holding the current video frame; null before the first
  ///        frame, without video, or on a headless renderer. Render thread.
  const dynamic_texture_ptr& texture() const noexcept {
    return texture_;
  }

  /// @brief Stops playback and frees the decoder, device and texture.
  void close();

 private:
  std::shared_ptr<playback> require() const;

  std::string name_;
  bison::key_t rmi_id_{};
  bison::synchronized<std::shared_ptr<playback>> pb_;
  dynamic_texture_ptr texture_;
  video_frame frame_; // reused upload buffer
};

/// @brief Registers the `__WishMediaPlayer` class in the `"wish"` namespace.
void register_media_player();

} // namespace media
} // namespace bdg::wish
