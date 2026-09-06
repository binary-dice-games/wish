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
///        author bumped `version`), or when its dockspace tree exists but
///        @p target_id has been rebuilt (via `build_dock_layout()`) by
///        someone else since @p layout_root last built it itself -- i.e. a
///        sibling app sharing the dockspace rebuilt it, wholesale, with its
///        own windows.
///
///        Rebuilds are tracked with an exact per-target counter for most
///        frames: ordinary user rearranging (dragging a window to a new
///        split, floating one out standalone, or a window mid drag-to-dock
///        with its `DockId` transiently detached) never calls
///        `build_dock_layout()` and so never moves it, regardless of how
///        `DockId` looks while or after it happens, and a same-process
///        sibling rebuild is caught immediately. That counter lives only in
///        memory, though, so it can't see a rebuild from a *previous*
///        process (e.g. running `docker`, then `git`, then `docker` again,
///        each a separate process sharing one imgui.ini and ambient
///        dockspace) -- the first check per layout each session instead
///        falls back to real, persisted window state (are any of its
///        windows still docked under @p target_id) to catch that case, then
///        establishes the counter baseline so later frames use the fast
///        path.
///        State is persisted in imgui.ini under `[WishDockLayout]`, keyed by
///        the layout's window-path list, not the dockspace id.
bool should_apply_dock_layout(const ui_element& layout_root, ImGuiID target_id, int32_t version);

/// @brief Record that @p layout_root was applied at @p version against
///        @p target_id's current rebuild generation, and mark imgui.ini
///        dirty so the version is persisted.
void note_dock_layout_applied(const ui_element& layout_root, ImGuiID target_id, int32_t version);

/// @brief Install the `[WishDockLayout]` `ImGuiSettingsHandler` on the
///        current ImGui context if not already present. Must be called
///        before the first `ImGui::NewFrame()` so the handler participates
///        in the initial `imgui.ini` load. Idempotent.
void install_dock_layout_settings_handler();

/// @brief Test-only seam: discard should_apply_dock_layout()'s in-memory
///        rebuild-generation bookkeeping (never persisted to imgui.ini in
///        production either) while leaving `applied_versions` and the
///        actual ImGui dock tree untouched. This is what genuinely differs
///        between "still the same process" and "a fresh process that
///        reloaded imgui.ini" -- see `should_apply_dock_layout()`'s
///        doc comment -- so tests use it to exercise that first-check
///        fallback path without a real process boundary.
void reset_dock_layout_generation_tracking_for_test();

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
