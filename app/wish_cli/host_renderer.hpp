// MIT License © 2025 Binary Dice Games
/**
 * @file host_renderer.hpp
 * @brief Shared host-window chrome (dockspace + menu bar) for imgui-based
 *        renderer backends.
 */
#pragma once

#include <context/context.hpp>
#include <imgui/imgui_desktop_shell.hpp>
#include <imgui/imgui_server_overlay.hpp>
#include <ui/ui_element.hpp>

#include <chrono>
#include <memory>
#include <vector>

namespace bdg::wish {

/**
 * @brief Non-template face of `host_renderer`, so the server app can turn
 *        on the server overlay without knowing the backend type.
 */
class server_overlay_host {
 public:
  virtual ~server_overlay_host() = default;

  /**
   * @brief Turns on the server overlay (see `server_overlay`) and posts a
   *        notification telling the operator its hotkey. Call before the
   *        server starts rendering. Null `info.shutdown` and
   *        `info.request_screenshot` default to this renderer's
   *        (closing it stops the server); `info.perf` is always this
   *        renderer's history.
   */
  virtual void enable_server_overlay(server_overlay_info info) = 0;
};

/**
 * @brief Extends a given imgui-based renderer backend (sdl3_renderer or
 *        web_renderer) with a fullscreen host window that provides:
 *          - A DockSpace so client windows can be docked anywhere in the
 *            host view.
 *          - A menu bar with host-level actions (including Quit),
 *            extensible by sessions that register a MenuBarExtension
 *            top-level object.
 *          - Optionally (enable_server_overlay()), the server overlay,
 *            toggled by a hotkey, drawn on top of every session.
 *          - Toast notifications (`renderer::notifications()`), drawn on
 *            top of everything.
 *        When a session registers a `DesktopShell` top-level object (the
 *        `wish desktop` bridge does), the menu bar is replaced by the
 *        desktop chrome instead -- wallpaper, desktop icons and a taskbar
 *        with a Start menu (see `render_desktop_shell()`).
 *
 * Templatized on the backend so the same host-window UI is shared by every
 * renderer that draws through ImGui, rather than duplicating it per backend.
 * Used by both `wish server` (one host shared by every connected session)
 * and `wish standalone` (one host for the single embedded session) --
 * standalone otherwise has no DockSpaceViewport for its app's windows to
 * dock into.
 *
 * `render_server_frame()` is defined out-of-line in host_renderer.cpp, which
 * explicitly instantiates `host_renderer<sdl3_renderer>` and
 * `host_renderer<web_renderer>` -- the only two specializations ever used --
 * so this header stays free of the ImGui-heavy render body as the chrome
 * grows, and callers only need the declaration to use the type.
 */
template <typename Base>
class host_renderer : public Base, public server_overlay_host {
 public:
  using Base::Base;

  void enable_server_overlay(server_overlay_info info) override;

  void render_server_frame(const std::vector<sync_context_ptr>& sessions) override;

  /// @brief Draws the server overlay (when open) and the notifications.
  void render_server_overlay(const std::vector<sync_context_ptr>& sessions) override;

  /// @brief Also true while the overlay is open or a notification is on
  ///        screen (clock, animations, expiry).
  bool wants_continuous_redraw() const override;

  /// @brief Also true when a notification was posted or removed, so a
  ///        notification posted while nothing else changes still shows.
  bool consume_render_request() override;

  /// @brief Times each frame for the performance history (perf()).
  void begin_frame() override;
  void end_frame() override;

  /// @brief Also closes one performance sample per second, even when no
  ///        frame is drawn.
  void tick(const std::vector<sync_context_ptr>& sessions) override;

  /// @brief Per-second performance history (render thread only).
  const perf_recorder& perf() const {
    return perf_;
  }

  /// @brief The overlay, or null when not enabled.
  server_overlay* overlay() {
    return overlay_.get();
  }

 private:
  /// Cross-frame UI state of the desktop chrome (see render_desktop_shell()).
  desktop_shell_state desktop_state_;
  std::unique_ptr<server_overlay> overlay_;
  bool notifications_visible_{false};
  perf_recorder perf_;
  std::chrono::steady_clock::time_point frame_start_{};
};

} // namespace bdg::wish
