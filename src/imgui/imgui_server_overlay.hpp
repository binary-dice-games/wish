// MIT License © 2026 Binary Dice Games
/// @file imgui_server_overlay.hpp
/// @brief The server overlay: a full-screen, Steam-overlay-like panel the
///        operator of a `wish server` opens with a hotkey (Shift+F1 by
///        default) on top of every client's windows.
///
/// Pages: an overview of the server and the local machine, the connected
/// clients, an editor for the settings described in the server store and
/// in the user stores (see docs/settings.md), the server log, and an About
/// page with links to the project. A bottom bar holds quick actions: open
/// the sandbox and log folders, the project links, and shutting the server
/// down. See docs/server-overlay.md.
#pragma once

#include <context/context.hpp>
#include <context/persistent_store.hpp>
#include <context/settings_store.hpp>
#include <server/machine_info.hpp>
#include <server/notifications.hpp>
#include <server/perf_stats.hpp>

#include <imgui.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Project home page, opened from the overlay's About page.
inline constexpr const char* kWishGithubUrl = "https://github.com/binary-dice-games/wish";
/// @brief Documentation index, opened from the overlay's About page.
inline constexpr const char* kWishDocsUrl = "https://github.com/binary-dice-games/wish/tree/main/docs";

/// @brief What the overlay shows about the server, and how it acts on it.
///        Filled by the server app (see `configured_server`).
struct server_overlay_info {
  std::string version; ///< wish version, e.g. "1.0".
  /// Key combination that toggles the overlay (parse_key_chord() syntax);
  /// empty or invalid disables the overlay.
  std::string hotkey{"Shift+F1"};
  std::string endpoint; ///< Where clients connect, e.g. "tcp 0.0.0.0:7070".
  std::string renderer; ///< e.g. "sdl3" or "web (http://127.0.0.1:8080)".
  std::filesystem::path log_file; ///< The server log.
  /// Persistent sandbox root (`--sandbox_root`); empty: each session gets a
  /// temporary directory.
  std::filesystem::path sandbox_root;
  /// The server's store registry; null hides the Settings page.
  store_registry_ptr stores;
  std::set<std::string> settings_admins; ///< `--settings_admins`.
  /// Stops the server (the "Shut down server" button); null hides it.
  std::function<void()> shutdown;
  /// Opens a file, folder or URL on the server's machine (default:
  /// open_in_host_shell(), on a background thread). Replaceable for tests.
  /// @return false when nothing could be opened.
  std::function<bool(const std::string&)> open_in_shell;

  /// Asks the renderer for a PNG of its next frame (see
  /// `renderer::request_screenshot()`); null hides the Screenshot button.
  std::function<bool(std::function<void(std::vector<uint8_t>)>)> request_screenshot;
  /// Where screenshots are saved (`--screenshot_dir`), created on demand.
  std::filesystem::path screenshot_dir;

  /// The host renderer's performance history; null hides the Performance
  /// page. Must outlive the overlay.
  const perf_recorder* perf{nullptr};

  /// Perfetto capture controls; all null when the server runs without
  /// `--profiling_dir` (the Performance page then says how to enable it).
  struct profiler_controls {
    std::filesystem::path dir; ///< Where traces are written.
    std::function<bool()> active; ///< Whether a capture is running.
    std::function<bool()> start; ///< Starts one; false on failure.
    std::function<void()> stop; ///< Stops and writes the trace.
  } profiler;
};

/// @brief One connected client, as listed on the Clients page.
struct overlay_client_info {
  bison::key_t session_id{0u};
  std::string identity; ///< Empty: anonymous.
  std::string language;
  std::filesystem::path sandbox;
  bool persistent_sandbox{false};
  bool settings_admin{false};
  bool has_user_store{false};
  std::chrono::system_clock::time_point connected_at;
  std::vector<std::string> windows; ///< Titles of its top-level windows.
};

/// @brief Copies the overlay's view of each session out of @p sessions,
///        locking each one briefly (read lock).
std::vector<overlay_client_info> collect_overlay_clients(const std::vector<sync_context_ptr>& sessions);

/**
 * @brief The server overlay. Owned by the host renderer; all methods run on
 *        the render thread inside an ImGui frame.
 */
class server_overlay {
 public:
  /// @brief Overlay pages, in tab order.
  enum class page { overview, clients, performance, settings, logs, about };

  /// @param info          What to show and how to act (see server_overlay_info).
  /// @param notifications Where action failures are reported; may be null.
  server_overlay(server_overlay_info info, notification_center_ptr notifications);
  ~server_overlay();

  server_overlay(const server_overlay&) = delete;
  server_overlay& operator=(const server_overlay&) = delete;

  /// @brief Whether a valid hotkey was configured (otherwise the overlay
  ///        can only be opened with set_open()).
  bool has_hotkey() const {
    return hotkey_ != 0;
  }

  /// @brief The configured hotkey text, e.g. "Shift+F1".
  const std::string& hotkey() const {
    return info_.hotkey;
  }

  bool is_open() const {
    return open_;
  }

  /// @brief Opens or closes the overlay (opening focuses it).
  void set_open(bool open);

  /// @brief The page shown; set_page() switches to another one.
  page current_page() const {
    return page_;
  }
  void set_page(page p) {
    page_ = p;
  }

  /**
   * @brief Toggles the overlay when its hotkey was pressed this frame, and
   *        closes it on Escape. Call early in the frame, before client
   *        sessions render, so the keys reach the overlay first.
   * @return true when the overlay was toggled.
   */
  bool handle_keys();

  /**
   * @brief Draws the overlay over everything drawn so far, when open.
   *        Call after every client session has rendered.
   * @param sessions The connected sessions.
   */
  void render(const std::vector<sync_context_ptr>& sessions);

  /// @brief Opens @p target via info's open_in_shell, reporting a failure
  ///        as an error notification. Public for the host's menu bar.
  void open_target(const std::string& target, const std::string& what);

  /**
   * @brief Takes a screenshot of the clients' windows: the overlay (and the
   *        notifications, see capturing()) hide while the next frame is
   *        captured, then the PNG is saved under `screenshot_dir` and a
   *        notification gives its path. The overlay comes back afterwards
   *        when it was open. No-op while a capture runs or without
   *        `request_screenshot`.
   */
  void take_screenshot();

  /// @brief Height of the bottom action bar while the overlay is shown
  ///        (0 when hidden), so toasts can stay clear of its buttons.
  float bottom_bar_height() const;

  /// @brief True while a screenshot is being taken: nothing of the
  ///        overlay's or the notifications' may be drawn.
  bool capturing() const {
    return capture_ != nullptr;
  }

 private:
  struct settings_scope; // one store: the server's or one identity's
  struct settings_row;
  struct capture_state; // shared with the screenshot callback

  void draw_backdrop();
  void draw_top_bar(size_t client_count);
  void draw_bottom_bar(const std::vector<overlay_client_info>& clients);
  void draw_overview(const std::vector<overlay_client_info>& clients);
  void draw_clients(const std::vector<overlay_client_info>& clients);
  void draw_settings(const std::vector<overlay_client_info>& clients);
  void draw_performance();
  void draw_logs();
  void draw_about();
  void draw_shutdown_popup(size_t client_count);

  void refresh_machine(bool force);
  void refresh_settings(const std::vector<overlay_client_info>& clients, bool force);
  void refresh_log(bool force);
  void update_capture();
  bool draw_setting_editor(settings_row& row);

  server_overlay_info info_;
  notification_center_ptr notifications_;
  ImGuiKeyChord hotkey_{0};
  bool open_{false};
  bool focus_next_{false};
  page page_{page::overview};
  std::chrono::steady_clock::time_point opened_at_{};
  std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};
  bool request_shutdown_popup_{false};

  // Overview: refreshed every couple of seconds while open.
  machine_info machine_;
  std::chrono::steady_clock::time_point machine_at_{};

  // Settings: "" selects the server store, else a user identity.
  std::string settings_identity_;
  std::string settings_category_;
  std::string settings_filter_;
  std::vector<std::string> known_identities_;
  std::unique_ptr<settings_scope> scope_;
  std::chrono::steady_clock::time_point settings_at_{};

  // Logs: the tail of the log file.
  std::vector<std::string> log_lines_;
  std::string log_filter_;
  bool log_follow_{true};
  bool log_scroll_to_end_{false};
  std::chrono::steady_clock::time_point log_at_{};
  std::filesystem::file_time_type log_mtime_{};

  // Screenshot in progress; null otherwise.
  std::shared_ptr<capture_state> capture_;

  // Performance: the newest trace in the profiler's directory.
  std::filesystem::path last_trace_;
  std::chrono::steady_clock::time_point trace_at_{};
};

namespace detail {

/// @brief The last @p max_lines lines of the text file @p path (reads at
///        most its last 256 KB); empty when it can't be read.
std::vector<std::string> read_log_tail(const std::filesystem::path& path, size_t max_lines);

/// @brief User identities with a store under @p store_dir (`users/<id>.bison`)
///        plus @p connected, sorted and unique; unsafe names are skipped.
std::vector<std::string> list_store_identities(const std::filesystem::path& store_dir,
                                               const std::vector<std::string>& connected);

/// @brief A new screenshot file name in @p dir for local time @p t:
///        "wish-YYYYMMDD-HHMMSS.png", with "-2", "-3", ... appended when
///        that file exists.
std::filesystem::path screenshot_path(const std::filesystem::path& dir, std::time_t t);

/// @brief The most recently modified `*.perfetto-trace` file in @p dir;
///        empty when there is none.
std::filesystem::path newest_trace(const std::filesystem::path& dir);

/// @brief "#RRGGBB" / "#RRGGBBAA" to RGBA floats; nullopt when malformed.
std::optional<std::array<float, 4>> parse_hex_color(const std::string& text);

/// @brief RGBA floats to "#RRGGBB", or "#RRGGBBAA" when @p with_alpha.
std::string to_hex_color(const float rgba[4], bool with_alpha);

} // namespace detail

} // namespace bdg::wish
