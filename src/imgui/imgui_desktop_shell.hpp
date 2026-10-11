// MIT License © 2026 Binary Dice Games
/// @file imgui_desktop_shell.hpp
/// @brief Desktop chrome (wallpaper, desktop icons, taskbar, Start menu,
///        tray, window switcher) drawn by a host renderer when a session
///        registers a `DesktopShell` top-level object.
#pragma once

#include <context/context.hpp>
#include <imgui/imgui_renderer.hpp>

#include <imgui.h>

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// @brief One launchable app of a DesktopShell, copied out of its
///        `DesktopApp` child (see desktop_shell.cpp for the fields).
struct desktop_app_info {
  bison::key_t node_id{0u}; ///< The DesktopApp node's `__wish_id`.
  std::string app;
  std::string label;
  std::string description;
  std::string collection;
  std::string icon;
  bool pinned{false};
  bool on_desktop{false};
  std::vector<int32_t> sessions; ///< Object groups of its running instances.
};

/// @brief One taskbar entry: a top-level Window, or an app's main frame (a
///        `DockSpaceViewport`) together with the other top-level windows of
///        the same app instance (same object group), which it stands for.
struct desktop_window_info {
  const sync_context* session{nullptr}; ///< Owning session (identity only).
  bison::key_t top_key{0u}; ///< Key in `top_level_objects`.
  /// Top-level keys minimized/raised with this entry (includes top_key).
  std::vector<bison::key_t> members;
  bool frame{false}; ///< top_key is a DockSpaceViewport.
  std::string title; ///< Display title (no `##` suffix).
  int app_index{-1}; ///< Index into the shell's apps, or -1.
  bool focused{false};
  bool minimized{false};
  bool closable{false};
};

/**
 * @brief UI state of the desktop chrome that must survive between frames
 *        (Start menu search text, switcher selection, window order, ...).
 *        Owned by the host renderer; opaque to it.
 */
struct desktop_shell_state {
  char search[128]{};
  bool focus_search{false};
  /// Most recently focused windows first; drives the switcher order.
  std::vector<std::pair<const sync_context*, bison::key_t>> mru;
  /// Taskbar order: windows in the order they first appeared.
  std::vector<std::pair<const sync_context*, bison::key_t>> taskbar_order;
  bool switcher_open{false};
  int switcher_index{0};
  /// Windows minimized by "show desktop", restored by its second click.
  std::vector<std::pair<const sync_context*, bison::key_t>> shown_desktop;
  std::string selected_icon;
  int calendar_year{0};
  int calendar_month{0}; ///< 0-11
  /// Sandbox-resolved absolute path per image src, so the per-frame icon
  /// lookups don't re-run path resolution; "" for a rejected src.
  std::unordered_map<std::string, std::string> resolved_paths;
  std::string resolved_for_dir; ///< resource_dir `resolved_paths` belongs to.
  bool nav_windowing_saved{false};
  ImGuiKeyChord saved_nav_next{0};
  ImGuiKeyChord saved_nav_prev{0};
};

/**
 * @brief Draws the desktop chrome if any session has a `DesktopShell`
 *        top-level object; otherwise draws nothing.
 *
 * Call once per frame from `renderer::render_server_frame()`, before the
 * sessions render. When it returns true it has drawn the whole host chrome:
 * the wallpaper, a bottom taskbar (a viewport side bar, so the work area
 * shrinks above it), desktop icons and a fullscreen passthrough dockspace
 * (published as the ambient dockspace), and has turned on the renderer's
 * floating window cascade (`imgui_renderer::set_desktop_cascade_area()`).
 * The caller must then not draw its own host window.
 *
 * Each session is locked (write lock) only briefly: once to copy out what
 * the chrome shows, and once more to apply what the user did (minimize,
 * raise, close, or a shell event for the owning client) -- never while
 * drawing, and never two at a time.
 *
 * @param r                The renderer drawing the frame.
 * @param sessions         Snapshot of the connected sessions.
 * @param state            Cross-frame UI state, owned by the caller.
 * @param shutdown_server  Run by the Start menu's "Shut down server" item.
 * @return true when a DesktopShell was found and drawn.
 */
bool render_desktop_shell(
    imgui_renderer& r,
    const std::vector<sync_context_ptr>& sessions,
    desktop_shell_state& state,
    const std::function<void()>& shutdown_server);

namespace detail {

/// @brief Display title of a window: @p title up to any `##`/`###` id
///        suffix, or @p fallback when that leaves nothing.
std::string desktop_display_title(const std::string& title, const std::string& fallback);

/// @brief Formats @p t with the strftime() @p format (local time); an empty
///        or invalid format gives "".
std::string desktop_format_time(const std::string& format, std::time_t t);

/// @brief How well @p app matches the Start menu search @p query: 0 = its
///        label starts with it (or the query is empty), 1 = label contains
///        it, 2 = description contains it, -1 = no match (case-insensitive).
int desktop_app_match_rank(const desktop_app_info& app, const std::string& query);

/// @brief Whether @p app matches the Start menu search @p query
///        (case-insensitive substring of its label or description; an
///        empty query matches everything).
bool desktop_app_matches(const desktop_app_info& app, const std::string& query);

/// @brief Collects the taskbar's entries from one session, with the app each
///        belongs to resolved through the session's object groups and each
///        app's `sessions` (@p apps; pass an empty list for a session other
///        than the shell's own). An object group (one client of a bridge)
///        with a `DockSpaceViewport` gets one entry per frame, standing for
///        all its top-level windows; otherwise every visible, non-modal
///        top-level `Window` is an entry of its own. Must be called with the
///        session locked.
std::vector<desktop_window_info>
desktop_collect_windows(const context& s, const sync_context* session, const std::vector<desktop_app_info>& apps);

/// @brief Copies a DesktopShell node's DesktopApp children, in order.
std::vector<desktop_app_info> desktop_collect_apps(const ui_element& shell);

/// @brief Number of days in @p month (0-11) of @p year.
int desktop_days_in_month(int year, int month);

/// @brief Weekday (0 = Monday ... 6 = Sunday) of the 1st of @p month.
int desktop_first_weekday(int year, int month);

} // namespace detail

} // namespace bdg::wish
