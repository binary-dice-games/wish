// MIT License © 2026 Binary Dice Games
/// @file mpv.hpp
/// @brief Server-side form for mpv, a media player: browse the client's files,
///        upload media to the sandbox, and play it back on the server.
#pragma once

#include "modules/bdg/common/server/tool_form.hpp"

#include <ui/ui_element.hpp>

#include "src/bison/bison_object.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <set>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Media player window: a **Local Machine** list (client-driven, like
///        mc's local panel), the sandbox's **Library** of media files, and a
///        player with a `Video` widget, a seek bar, transport buttons, volume
///        and loop.
///
/// The form owns the UI and the library (it scans the sandbox itself). It
/// never decodes anything: playback runs in the server's media service, and
/// the *client* opens and drives the player through the client media API
/// (`wish_app_host::open_media()`), because that is the API every app uses
/// (and works the same over any transport). So the form asks the client:
///
/// Emitted events:
///   - `on_local_navigate` `{name, type}` -- show another local directory:
///     `type` `"path"` (name is a full path), `"dir"` (a child, or ".."). The
///     client answers with update_local_listing().
///   - `on_upload_requested` `{names: [..], local_path, dest, play, player,
///     loop, volume}` -- upload these local files into sandbox folder `dest`
///     (`media`), then call refresh_library(); when `play`, open the first
///     one as in on_open_requested.
///   - `on_open_requested` `{path, player, loop, volume}` -- open sandbox file
///     `path` as media player `player` (the name the Video widget shows) and
///     play it.
///   - `on_control` `{action, value}` -- `action` is `"play"`, `"pause"`,
///     `"stop"`, `"seek"` (value: seconds), `"volume"` (0..1), `"loop"` (0/1)
///     or `"close"` (the file is about to be removed).
///   - `closed` -- the window was closed; the UI is gone.
///
/// RMI methods (called by the client):
///   - `update_local_listing` `{path, files: [{name, type, size}]}`
///   - `update_status` `{state, position, duration, has_video, has_audio,
///     audio_output}` -- the open player's status (see media_player).
///   - `media_unavailable` `{}` -- the server plays no media.
///   - `refresh_library` `{}` -- rescan the sandbox.
class mpv : public common::tool_form {
 public:
  explicit mpv(bison::dynamic&& base);
  ~mpv() override;

  /// File extensions the Library and the Local list show (lower case).
  static bool is_media_name(const std::string& name);

  /// Media files under @p root (relative, '/'-separated, sorted), skipping
  /// the session's built-in `res/` and tools' `private/` folders.
  static std::vector<std::pair<std::string, uintmax_t>> scan_library(const std::filesystem::path& root);

  /// "1:05" / "1:02:03".
  static std::string format_time(double seconds);

  bison::dynamic do_update_local_listing(const bison::dynamic& args);
  bison::dynamic do_update_status(const bison::dynamic& args);
  bison::dynamic do_media_unavailable(const bison::dynamic& args);
  bison::dynamic do_refresh_library(const bison::dynamic& args);
  bison::dynamic on_set(const bison::dynamic& patch);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct local_entry {
    std::string name;
    bool is_dir{false};
    std::string size;
  };

  void fill_local();
  void fill_library();
  void select_local(int32_t index, bool ctrl, bool shift);
  void activate_local(int32_t index);
  void request_upload(const std::vector<std::string>& names, bool play);
  void open_library(int32_t index);
  void remove_selected();
  void control(const std::string& action, float value = 0.0f);
  void set_status(const std::string& text, bool ok = true);
  void show_player_controls(bool playing);
  void clear_rows(const ui_element_ptr& table, std::vector<bison::key_t>& ids);

  std::string player_name_;
  bison::key_t window_id_{};

  // Widgets
  ui_element_ptr local_path_, local_table_, local_selected_;
  ui_element_ptr library_table_, library_count_;
  ui_element_ptr now_playing_, video_, seek_, time_, status_, volume_, loop_;
  ui_element_ptr play_btn_, unavailable_;
  bison::key_t local_path_id_{}, local_table_id_{}, up_id_{}, upload_id_{}, upload_play_id_{};
  bison::key_t library_table_id_{}, lib_play_id_{}, lib_remove_id_{}, lib_refresh_id_{};
  bison::key_t play_id_{}, stop_id_{}, seek_id_{}, volume_id_{}, loop_id_{}, video_id_{};

  // State
  std::string local_dir_;
  std::vector<local_entry> local_entries_;
  std::set<std::string> local_selection_;
  int32_t local_anchor_{-1};
  std::vector<bison::key_t> local_row_ids_;
  std::vector<std::pair<std::string, uintmax_t>> library_;
  int32_t library_selected_{-1};
  std::vector<bison::key_t> library_row_ids_;
  std::string playing_path_; ///< sandbox path of the open file; empty: none
  bool playing_{false};
  bool media_available_{true};
  /// update_status() leaves the seek bar alone this long after the user
  /// moved it, so the bar doesn't jump back while the seek is applied.
  std::chrono::steady_clock::time_point seek_quiet_until_{};
};

/// @brief Registers the `Mpv` form class in the `"wish"` namespace.
void register_mpv();

} // namespace bdg::wish
