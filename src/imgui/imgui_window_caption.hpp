// MIT License © 2026 Binary Dice Games
/// @file imgui_window_caption.hpp
/// @brief Windows-style title bar ("caption") for an app's top-level window
///        on a `wish desktop`: title, minimize / maximize / close buttons,
///        and a border with a drop shadow so overlapping windows stay
///        distinguishable.
///
/// Used by `render_window()` and `render_dockspace_viewport()` for an app's
/// main window while a desktop shell is active. The caller begins the ImGui
/// window with a hidden title (`"###id"`), so the title text is drawn here,
/// ellipsized before the buttons, instead of running underneath them.
#pragma once

#include <imgui.h>

namespace bdg::wish {

/// @brief What the user did with the caption this frame.
enum class caption_action {
  none,
  minimize,
  /// Maximize button, or a double-click on the title.
  toggle_maximize,
  close,
  /// "Reset layout" in the title bar's context menu.
  reset_layout,
};

/// @brief Which caption buttons are enabled, and how the maximize one looks.
struct caption_options {
  /// The window can be resized: false greys the maximize button out.
  bool can_maximize = true;
  /// Draw the "restore" glyph instead of the "maximize" one.
  bool maximized = false;
  /// False greys the close button out.
  bool can_close = true;
  /// Offer "Reset layout" in the context menu (an app frame with panels).
  bool can_reset_layout = false;
};

/// @brief Whether the ImGui window @p label was docked at its last render;
///        false for a window ImGui has not seen yet. Decides, before
///        `ImGui::Begin()`, whether this frame shows the caption (floating)
///        or ImGui's own dock tab (docked), which needs the visible title.
bool window_was_docked(const char* label);

/// @brief Draws the caption into the title bar of the current window: call
///        right after `ImGui::Begin()`. Draws nothing and returns `none`
///        for a docked, collapsed or title-less window. A right-click on
///        the title opens a context menu (Maximize/Restore, Minimize, Reset
///        layout, Close), like a Windows window's system menu.
/// @param title  Text shown in the title bar.
/// @param opts   Enabled buttons / maximize glyph.
/// @return The button clicked this frame, if any.
caption_action draw_window_caption(const char* title, const caption_options& opts);

} // namespace bdg::wish
