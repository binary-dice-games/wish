// MIT License © 2026 Binary Dice Games
/// @file imgui_dock_layout.cpp
/// @brief DockBuilder realization + version persistence for `DockLayout`.
#include "imgui_dock_layout.hpp"

#ifdef WISH_IMGUI_ENABLED

#include <context/context.hpp>
#include <context/logger.hpp>
#include <imgui/imgui_ui_renderer.hpp>
#include <ui/ui_element.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

namespace {

void log_warn(logger* log, const std::string& msg) {
  if (log)
    log->warn(msg);
}

ImGuiDir dir_from_string(const std::string& s, bool& ok) {
  ok = true;
  if (s == "left")
    return ImGuiDir_Left;
  if (s == "right")
    return ImGuiDir_Right;
  if (s == "up")
    return ImGuiDir_Up;
  if (s == "down")
    return ImGuiDir_Down;
  ok = false;
  return ImGuiDir_Left;
}

// Split @p text on '\n' into non-empty, whitespace-trimmed tokens.
std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    size_t end = (nl == std::string::npos) ? text.size() : nl;
    size_t a = start;
    size_t b = end;
    while (a < b && std::isspace(static_cast<unsigned char>(text[a])))
      ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1])))
      --b;
    if (b > a)
      out.push_back(text.substr(a, b - a));
    if (nl == std::string::npos)
      break;
    start = nl + 1;
  }
  return out;
}

// Recursively realize one DockSplit/DockArea node into @p node_id.
// Returns false on any malformed node (aborting the whole build).
bool realize_node(const ui_element& node, ImGuiID node_id, logger* log) {
  const key_t cls = node.class_key();

  if (cls == "DockArea"_key) {
    const auto& area = static_cast<const ui_dock_area&>(node);
    const std::vector<std::string> windows = split_lines(area.windows_ref());
    if (windows.empty()) {
      log_warn(log, "wish DockLayout: DockArea has no windows");
      return false;
    }
    const std::string& focused = area.focused_ref();
    for (const auto& w : windows) {
      if (w == focused)
        continue; // docked last, below, so it wins the active tab
      ImGui::DockBuilderDockWindow(w.c_str(), node_id);
    }
    if (!focused.empty())
      ImGui::DockBuilderDockWindow(focused.c_str(), node_id);
    return true;
  }

  if (cls == "DockSplit"_key) {
    const auto& split = static_cast<const ui_dock_split&>(node);
    bool dir_ok = false;
    const ImGuiDir dir = dir_from_string(split.dir_ref(), dir_ok);
    if (!dir_ok) {
      log_warn(log, "wish DockLayout: DockSplit has bad dir '" + split.dir_ref() + "'");
      return false;
    }
    float ratio = split.ratio(0.5f);
    if (!(ratio > 0.0f && ratio < 1.0f))
      ratio = 0.5f;

    std::vector<const ui_element*> kids;
    node.for_each_child_ordered([&](key_t, ui_element& c) { kids.push_back(&c); });
    if (kids.size() != 2) {
      log_warn(log, "wish DockLayout: DockSplit needs exactly 2 children, got " + std::to_string(kids.size()));
      return false;
    }

    ImGuiID id_dir = 0;
    ImGuiID id_opposite = 0;
    ImGui::DockBuilderSplitNode(node_id, dir, ratio, &id_dir, &id_opposite);
    return realize_node(*kids[0], id_dir, log) && realize_node(*kids[1], id_opposite, log);
  }

  log_warn(log, "wish DockLayout: unexpected child class in layout tree");
  return false;
}

// Visit every DockArea in a DockLayout tree, calling fn(window_path) for each
// listed window in order.
template <typename Fn>
void for_each_layout_window(const ui_element& node, Fn&& fn) {
  if (node.class_key() == "DockArea"_key) {
    for (const auto& w : split_lines(static_cast<const ui_dock_area&>(node).windows_ref()))
      fn(w);
  }
  node.for_each_child_ordered([&](key_t, ui_element& c) { for_each_layout_window(c, fn); });
}

// A stable identity for one DockLayout, independent of the dockspace it
// targets: the hash of its window-path list. Two different apps sharing the
// same ambient dockspace (docker vs kubectl vs git, all docking into the
// host chrome's HostDockSpace) get distinct identities, so each tracks its
// own applied version and neither blocks the other.
ImGuiID layout_identity(const ui_element& layout_root) {
  std::string all;
  for_each_layout_window(layout_root, [&](const std::string& w) {
    all += w;
    all += '\n';
  });
  return ImHashStr(all.c_str(), all.size());
}

// True iff AT LEAST ONE window the layout names is currently docked into a
// live node whose root is target_id -- i.e. this layout still has some
// footprint in target_id's dockspace, so it hasn't been fully displaced.
// False only when NONE of its windows are there any more (a sibling app
// that shares the dockspace rebuilt it from scratch, replacing every one of
// this layout's windows with its own).
//
// Deliberately "any", not "all": a user can drag one of this layout's
// windows out to float it standalone (a normal, supported rearrangement,
// same as moving it to a different split) while every other window in the
// layout stays docked under target_id. That must not look like a sibling
// rebuild -- an "all must still be docked" check would treat the one
// floated window as proof the whole arrangement was displaced and rebuild
// the hard-coded default over the *other*, untouched windows too. A genuine
// sibling rebuild replaces the dockspace wholesale, so it always clears
// every one of this layout's windows at once -- "any" still correctly
// catches that.
//
// This reads real, persisted state (window->DockId, restored by ImGui
// itself from imgui.ini at startup) rather than any in-process bookkeeping,
// which is what makes it able to detect a rebuild that happened in a
// *previous process* -- see the comment on target_generations() below for
// why that matters and isn't covered by the generation counter alone.
bool layout_windows_are_live(const ui_element& layout_root, ImGuiID target_id) {
  bool any_named = false;
  bool any_live = false;
  for_each_layout_window(layout_root, [&](const std::string& path) {
    any_named = true;
    if (any_live)
      return;
    const ImGuiID wid = ImHashStr(path.c_str(), path.size());
    ImGuiID dock_id = 0;
    if (ImGuiWindow* w = ImGui::FindWindowByID(wid))
      dock_id = w->DockId;
    else if (ImGuiWindowSettings* ws = ImGui::FindWindowSettingsByID(wid))
      dock_id = ws->DockId;
    ImGuiDockNode* node = dock_id ? ImGui::DockBuilderGetNode(dock_id) : nullptr;
    if (node && ImGui::DockNodeGetRootNode(node)->ID == target_id)
      any_live = true;
  });
  return any_named && any_live;
}

// ── Applied-version persistence ([WishDockLayout] in imgui.ini) ──────────────
//
// The map is keyed by layout_identity() (NOT the dockspace id -- see that
// helper). It is process-global (wish drives one ImGui context) but guarded
// by the context pointer it was populated for, so a test that destroys and
// recreates the context starts clean instead of seeing a previous test's
// "already applied" entries.

std::unordered_map<ImGuiID, int32_t>& applied_versions() {
  static std::unordered_map<ImGuiID, int32_t> m;
  return m;
}

// ── Rebuild-generation tracking (in-memory only, not persisted) ─────────────
//
// should_apply_dock_layout()'s steady-state check -- "has target_id been
// rebuilt since I last touched it" -- has two possible sources of truth:
//
//  1. An exact, in-process counter bumped once per build_dock_layout() call
//     (the only place that ever tears down and rebuilds target_id). Cheap,
//     instant, and immune to ordinary user rearranging (dragging, dropping,
//     floating a window out never calls build_dock_layout(), so they never
//     move it) -- but it lives only in memory, so it knows nothing about
//     rebuilds from *before this process started*, e.g. a different wish
//     tool (docker, then git, then docker again -- each its own process)
//     sharing the same on-disk imgui.ini and the same ambient dockspace id.
//     To that counter, "0 == 0" looks identical whether nothing has ever
//     touched target_id, or a sibling process rebuilt it five minutes ago
//     right before this one started.
//
//  2. layout_windows_are_live() above, which reads real, persisted window
//     state and so sees a cross-process rebuild correctly -- but is a
//     heuristic over DockId, which a mid-drag or just-dropped window can
//     transiently misrepresent (see should_apply_dock_layout()'s use of
//     both).
//
// The fix is to use each for what it's good at: trust the fast, exact
// counter once this session has established a baseline for a layout (via
// building it, or via a real liveness check below), and fall back to real
// liveness only on the first check per layout per session -- which is
// exactly the moment a previous process's rebuild would otherwise go
// undetected.
std::unordered_map<ImGuiID, uint64_t>& target_generations() {
  static std::unordered_map<ImGuiID, uint64_t> m;
  return m;
}
std::unordered_map<ImGuiID, uint64_t>& last_seen_generations() {
  static std::unordered_map<ImGuiID, uint64_t> m;
  return m;
}

ImGuiContext*& installed_ctx() {
  static ImGuiContext* c = nullptr;
  return c;
}

void reset_if_new_context() {
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (installed_ctx() != g) {
    applied_versions().clear();
    target_generations().clear();
    last_seen_generations().clear();
    installed_ctx() = g;
  }
}

// Entries are read one at a time -- ReadOpen for "[WishDockLayout][id]", then
// every ReadLine for that entry, then the next ReadOpen -- so a single
// file-static "id currently being read" is sufficient and simplest (ImGui's
// own handlers use a pool of heap cookies instead; unnecessary here).
ImGuiID g_reading_id = 0;

void* dl_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
  unsigned int id = 0;
  if (std::sscanf(name, "0x%X", &id) != 1)
    return nullptr;
  g_reading_id = static_cast<ImGuiID>(id);
  return &g_reading_id; // any non-null cookie
}

void dl_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
  if (!entry)
    return;
  int version = 0;
  if (std::sscanf(line, "Version=%d", &version) == 1)
    applied_versions()[g_reading_id] = version;
}

void dl_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
  for (const auto& [id, version] : applied_versions()) {
    buf->appendf("[%s][0x%08X]\n", handler->TypeName, id);
    buf->appendf("Version=%d\n\n", version);
  }
}

void dl_ClearAll(ImGuiContext*, ImGuiSettingsHandler*) {
  applied_versions().clear();
  target_generations().clear();
  last_seen_generations().clear();
}

} // namespace

bool build_dock_layout(const ui_element& layout_root, ImGuiID target_id, ImVec2 node_size, logger* log) {
  const ui_element* root_child = nullptr;
  layout_root.for_each_child_ordered([&](key_t, ui_element& c) {
    if (!root_child)
      root_child = &c;
  });
  if (!root_child) {
    log_warn(log, "wish DockLayout: no DockSplit/DockArea child");
    return false;
  }

  ImGui::DockBuilderRemoveNode(target_id);
  ImGui::DockBuilderAddNode(target_id, ImGuiDockNodeFlags_DockSpace);
  if (node_size.x > 0.0f && node_size.y > 0.0f)
    ImGui::DockBuilderSetNodeSize(target_id, node_size);

  // target_id's tree just got torn down and rebuilt from scratch -- bump its
  // generation so should_apply_dock_layout() can tell any layout that was
  // built against the old tree that it's now stale (see the comment on
  // target_generations() above). This is the ONLY place that happens.
  ++target_generations()[target_id];

  const bool ok = realize_node(*root_child, target_id, log);
  ImGui::DockBuilderFinish(target_id);
  return ok;
}

bool should_apply_dock_layout(const ui_element& layout_root, ImGuiID target_id, int32_t version) {
  reset_if_new_context();

  // Never applied at this version (covers first run and an author's bump).
  auto it = applied_versions().find(layout_identity(layout_root));
  if (it == applied_versions().end() || version > it->second)
    return true;

  // No dockspace tree at all yet.
  if (ImGui::DockBuilderGetNode(target_id) == nullptr)
    return true;

  const ImGuiID id = layout_identity(layout_root);

  // Fast, exact path: if this session already has a generation baseline for
  // this layout (from building it itself, or from a real liveness check
  // below) and target_id's rebuild counter hasn't moved since, nothing has
  // touched it -- true regardless of how many times a user has dragged or
  // floated windows within it this session, since neither ever bumps the
  // counter. See target_generations()'s comment for why this alone isn't
  // enough on the very first check.
  auto gen_it = last_seen_generations().find(id);
  if (gen_it != last_seen_generations().end() && gen_it->second == target_generations()[target_id])
    return false;

  // A window is mid drag-to-dock (title bar or tab drag): ImGui transiently
  // detaches it from its dock node for the gesture's duration. Only
  // reachable here on a layout's first check this session (the fast path
  // above handles every later frame), but guard it anyway rather than let
  // the liveness fallback below possibly misread it as a sibling's rebuild.
  if (GImGui && GImGui->MovingWindow != nullptr)
    return false;

  // No generation baseline yet this session for an already-applied layout:
  // either truly the first frame after startup, or -- the case that
  // matters -- this process just started and a *different* process last
  // touched target_id (a sibling wish tool sharing the same ambient
  // dockspace, since exited). The in-memory counter can't see that, so
  // fall back to real, persisted state: are any of this layout's windows
  // still, in fact, docked under target_id?
  if (layout_windows_are_live(layout_root, target_id)) {
    // Establish the baseline now so every later frame this session takes
    // the fast path above instead of re-deriving this from window state.
    last_seen_generations()[id] = target_generations()[target_id];
    return false;
  }
  return true;
}

void note_dock_layout_applied(const ui_element& layout_root, ImGuiID target_id, int32_t version) {
  reset_if_new_context();
  const ImGuiID id = layout_identity(layout_root);
  applied_versions()[id] = version;
  last_seen_generations()[id] = target_generations()[target_id];
  ImGui::MarkIniSettingsDirty();
}

void reset_dock_layout_generation_tracking_for_test() {
  target_generations().clear();
  last_seen_generations().clear();
}

void install_dock_layout_settings_handler() {
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g)
    return;
  reset_if_new_context();
  if (ImGui::FindSettingsHandler("WishDockLayout") != nullptr)
    return;
  ImGuiSettingsHandler h;
  h.TypeName = "WishDockLayout";
  h.TypeHash = ImHashStr("WishDockLayout");
  h.ClearAllFn = dl_ClearAll;
  h.ReadOpenFn = dl_ReadOpen;
  h.ReadLineFn = dl_ReadLine;
  h.WriteAllFn = dl_WriteAll;
  ImGui::AddSettingsHandler(&h);
}

// ── render_dock_layout (dispatch entry) ─────────────────────────────────────
//
// Defined here rather than in imgui_ui_renderer.cpp so the id-resolution
// (`ImGuiWindow::GetID`) and DockBuilder calls all stay in the one TU that
// includes imgui_internal.h. Declared in imgui_ui_renderer.hpp with every
// other render_* function; wired into built_in_render_fns() in
// imgui_renderer.cpp.
void render_dock_layout(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_dock_layout&>(node0);

  const std::string& target = node.target_ref();
  ImGuiID target_id = 0;
  if (target.empty()) {
    target_id = r.ambient_dockspace_id();
  } else if (ImGuiWindow* w = GImGui ? GImGui->CurrentWindow : nullptr) {
    // A named target seeds the id the same way render_dockspace() does --
    // hashed against the current window. Only reachable when the DockLayout
    // renders inside a window (e.g. as a DockSpaceViewport child).
    target_id = w->GetID(target.c_str());
  }
  if (target_id == 0)
    return; // no ambient dockspace this frame and no resolvable target

  const int32_t version = node.version(1);
  if (!should_apply_dock_layout(node, target_id, version))
    return;

  const ImVec2 size = ImGui::GetMainViewport()->WorkSize;
  if (build_dock_layout(node, target_id, size, s.logger_service.get()))
    note_dock_layout_applied(node, target_id, version);
}

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
