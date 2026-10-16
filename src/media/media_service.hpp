// MIT License © 2026 Binary Dice Games
/// @file media_service.hpp
/// @brief Per-session RMI service that plays audio/video files from the
///        session sandbox on the server.
#pragma once

#include <context/context.hpp>
#include <media/media_player.hpp>
#include <server/renderer.hpp>

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::media {

class media_service;
using media_service_ptr = std::shared_ptr<media_service>;

/**
 * @brief Opens and owns a session's media players.
 *
 * Registered in the `"wish"` bison namespace as `"__WishMedia"`; the server
 * attaches one instance to every session (see `server::on_session_created()`).
 * The server decodes and plays everything: the client only sends commands,
 * so it needs no media library.
 *
 * ## RMI methods exposed to clients
 *
 * | Method  | Params | Result |
 * |---------|--------|--------|
 * | `open`  | `"name"`, `"src"`: string (required); `"autoplay"`, `"loop"`: bool; `"volume"`: float | `"id"`: the player's RMI object id, `"has_video"`, `"has_audio"`: bool, `"width"`, `"height"`: int32, `"duration"`: float |
 * | `close` | `"name"`: string | `"closed"`: bool |
 * | `list`  | – | `"players"`: list of names |
 *
 * ## Sandbox and limits
 *
 * - `src` must name a file already in the session's sandbox (uploaded with
 *   `upload_file`/`sync_resource_package`). It is resolved with
 *   `file_service::resolve_path()`: absolute paths are refused unless the
 *   server allows them, `..` escapes always are, and URLs are not fetched.
 *   The decoder then reads only that file (see `media_decoder`).
 * - At most `kMaxPlayers` players per session; names are at most
 *   `kMaxName` bytes. Opening an existing name replaces that player.
 * - Every player stops when the session ends.
 */
class media_service : public bison::dynamic {
 public:
  static constexpr size_t kMaxPlayers = 8;
  static constexpr size_t kMaxName = 128;

  /**
   * @param resource_dir          The session sandbox.
   * @param allow_absolute_paths  `context::allow_absolute_paths`.
   * @param defaults              Applied to every player (tests turn the
   *                              audio device off or inject a clock).
   */
  static media_service_ptr instantiate(
      std::filesystem::path resource_dir, bool allow_absolute_paths, playback_options defaults = {});

  media_service(bison::dynamic&& base, std::filesystem::path resource_dir, bool allow_absolute_paths,
      playback_options defaults);

  /// @brief Closes every player.
  ~media_service() override;

  media_service(const media_service&) = delete;
  media_service& operator=(const media_service&) = delete;

  /// @brief Points the service at a new sandbox (after authentication moved
  ///        the session to a persistent directory).
  void set_resource_dir(std::filesystem::path resource_dir);

  /// @brief The RMI context the `open` method files new players in (see
  ///        `detail::find_singleton_service()`); without one, the context
  ///        of the current dispatch (`detail::current_context`) is used.
  void set_rmi_context(bison::rmi::context* ctx) noexcept {
    rmi_ctx_.store(ctx, std::memory_order_release);
  }

  /**
   * @brief Opens @p src as player @p name (see class doc for the rules).
   * @param params  `"autoplay"`, `"loop"`, `"volume"` as in the RMI call.
   * @throws std::invalid_argument for a bad name/path or too many players;
   *         std::runtime_error when the file isn't playable.
   */
  media_player_ptr open(const std::string& name, const std::string& src, const bison::dynamic& params = {});

  /// @brief Closes player @p name. @return Whether it existed.
  bool close(const std::string& name);

  /// @brief Player @p name, or null.
  media_player_ptr find(const std::string& name) const;

  /// @brief Names of the open players.
  std::vector<std::string> names() const;

  /// @brief True when no player is open (lets the render loop skip it).
  bool empty() const;

  /**
   * @brief Advances every player (frames, events). Render thread, with the
   *        session locked.
   * @return True when the session should be redrawn.
   */
  bool pump(const context& s, renderer& r);

 private:
  bison::synchronized<std::filesystem::path> resource_dir_;
  bool allow_absolute_paths_;
  playback_options defaults_;
  bison::synchronized<std::map<std::string, media_player_ptr>> players_;
  std::atomic<bison::rmi::context*> rmi_ctx_{nullptr};
};

/// @brief Registers `__WishMedia` and `__WishMediaPlayer` in `"wish"`.
void register_media_service();

/**
 * @brief Runs `media_service::pump()` for every session that has players,
 *        marking the ones that changed dirty. Called by the render loop on
 *        every iteration, whether or not a frame is drawn, so audio-only
 *        players still report their end.
 */
void pump_sessions(renderer& r, const std::vector<sync_context_ptr>& sessions);

} // namespace bdg::wish::media
