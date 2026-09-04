// MIT License © 2026 Binary Dice Games
/// @file imgui_dock_layout.hpp
/// @brief DockBuilder realization of a `DockLayout` element tree.
///
/// `imgui_dock_layout.cpp` is the ONLY `src/imgui/*` translation unit that
/// includes `imgui_internal.h` (for the `ImGui::DockBuilder*` API and the
/// `ImGuiSettingsHandler` used to persist applied layout versions). Every
/// other renderer source deliberately stays on the public API -- see the
/// comment near `SplitterBehavior` in `imgui_ui_renderer.cpp`. The
/// `render_dock_layout()` dispatch function (declared with every other
/// `render_*` in `imgui_ui_renderer.hpp`) is defined in that .cpp too, so
/// all DockBuilder / id-resolution code stays quarantined in the one file.
#pragma once

#ifdef WISH_IMGUI_ENABLED

#include <imgui.h>

#include <cstdint>

namespace bdg::wish {

class ui_element;
class logger;

/// @brief Realize @p layout_root (a `DockLayout` element and its
///        `DockSplit`/`DockArea` children) as an ImGui DockBuilder node tree
///        rooted at @p target_id.
///
/// Removes any existing node at @p target_id, rebuilds the split tree, and
/// docks every `Window` named in a `DockArea` into its node. Caller is
/// responsible for deciding *whether* to apply (see
/// `should_apply_dock_layout()`); this function always rebuilds.
///
/// @param layout_root  The `DockLayout` element. Its single child is walked.
/// @param target_id    Dockspace id to build into (never 0 -- caller checks).
/// @param node_size    Size to give the root node (typically the viewport
///                     work size).
/// @param log          Optional session logger for malformed-tree warnings.
/// @return true if a tree was built; false if the child tree was missing or
///         malformed (in which case an empty node is left at @p target_id).
bool build_dock_layout(const ui_element& layout_root, ImGuiID target_id, ImVec2 node_size, logger* log);

/// @brief Whether @p layout_root should be (re)applied this run. True when it
///        has never been applied at @p version (fresh imgui.ini, or the
///        author bumped `version`), or when its dockspace tree exists but the
///        windows it names are not the ones currently laid out under
///        @p target_id (a sibling app sharing the dockspace rebuilt it).
///        False once its arrangement is live, so a user's own rearrangement
///        is left alone. State is persisted in imgui.ini under
///        `[WishDockLayout]`, keyed by the layout's window-path list, not the
///        dockspace id.
bool should_apply_dock_layout(const ui_element& layout_root, ImGuiID target_id, int32_t version);

/// @brief Record that @p layout_root was applied at @p version and mark
///        imgui.ini dirty so it is persisted.
void note_dock_layout_applied(const ui_element& layout_root, int32_t version);

/// @brief Install the `[WishDockLayout]` `ImGuiSettingsHandler` on the
///        current ImGui context if not already present. Must be called
///        before the first `ImGui::NewFrame()` so the handler participates
///        in the initial `imgui.ini` load. Idempotent.
void install_dock_layout_settings_handler();

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
