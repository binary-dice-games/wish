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

// A stable identity for one DockLayout: the hash of its window-path list
// *plus* the dockspace it targets. Two different apps sharing the same
// ambient dockspace (docker vs kubectl vs git, all docking into the host
// chrome's HostDockSpace) already get distinct identities from having
// different window sets, so each tracks its own applied version and
// neither blocks the other -- folding target_id in on top of that doesn't
// change that. What it does fix: an app switching its *own* layout from
// the ambient dockspace to a named, nested one (e.g. opting into
// `dock::viewport(...)` -- see PLAN.md's "Isolating an app's dockspace
// from siblings") keeps the exact same window-path list and version
// number, so without target_id the switch is indistinguishable from
// "already applied" -- and the stale imgui.ini record then blocks
// `should_apply_dock_layout()` from ever running `build_dock_layout()`
// against the new node. Worse, its one escape hatch (re-apply if the
// target node doesn't exist) never fires either: `ImGui::DockSpace()`
// (called by `render_dockspace_viewport()` just before its children,
// including this `DockLayout`, render) auto-creates an empty node at the
// new target_id as a side effect *before* this check runs, so
// `DockBuilderGetNode(target_id)` is already non-null the very first
// frame -- silently discarding the layout and leaving every window
// undocked. Hashing target_id in makes a changed target a new identity,
// exactly like a changed window-path list already was.
ImGuiID layout_identity(const ui_element& layout_root, ImGuiID target_id) {
  std::string all;
  for_each_layout_window(layout_root, [&](const std::string& w) {
    all += w;
    all += '\n';
  });
  char buf[16];
  std::snprintf(buf, sizeof(buf), "@%08X", target_id);
  all += buf;
  return ImHashStr(all.c_str(), all.size());
}

// ── Applied-version persistence ([WishDockLayout] in imgui.ini) ──────────────
//
// The map is keyed by layout_identity() (NOT the dockspace id -- see that
// helper). It is the ONLY state should_apply_dock_layout() consults: has
// this exact layout, at this `version`, ever been applied before, in this
// process or a prior one? Persisted in imgui.ini, so it's a real "have I
// ever seen this before" fact, not something reconstructed from a moment's
// worth of live ImGui docking state that a drag, a float-out, or a sibling
// app's rebuild can all make transiently look the same. It is
// process-global (wish drives one ImGui context) but guarded by the context
// pointer it was populated for, so a test that destroys and recreates the
// context starts clean instead of seeing a previous test's "already
// applied" entries.

std::unordered_map<ImGuiID, int32_t>& applied_versions() {
  static std::unordered_map<ImGuiID, int32_t> m;
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

  const bool ok = realize_node(*root_child, target_id, log);
  ImGui::DockBuilderFinish(target_id);
  return ok;
}

bool should_apply_dock_layout(const ui_element& layout_root, ImGuiID target_id, int32_t version) {
  reset_if_new_context();

  // Never applied at this version (covers first run and an author's bump).
  auto it = applied_versions().find(layout_identity(layout_root, target_id));
  if (it == applied_versions().end() || version > it->second)
    return true;

  // Already applied at this version -- leave it alone, full stop. No matter
  // what has happened to target_id since (the user rearranged it, floated a
  // window out, is mid-drag, or a sibling app sharing the dockspace rebuilt
  // it wholesale with its own windows): once applied_versions() says this
  // exact layout has been applied at this version, ImGui's own imgui.ini
  // persistence is what owns the arrangement from here on, the same as any
  // window the user dragged by hand. The only exception is a dockspace tree
  // that doesn't exist at all (e.g. a hand-edited imgui.ini with no
  // [Docking] data) -- rare, but worth a real dockspace over silently
  // leaving every window undocked.
  return ImGui::DockBuilderGetNode(target_id) == nullptr;
}

void note_dock_layout_applied(const ui_element& layout_root, ImGuiID target_id, int32_t version) {
  reset_if_new_context();
  applied_versions()[layout_identity(layout_root, target_id)] = version;
  ImGui::MarkIniSettingsDirty();
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
    // A named target must reproduce the id render_dockspace_viewport()
    // published for its own nested ImGui::DockSpace(id) call -- computed as
    // ImGui::GetID(id) *immediately* after Begin(), before any child has
    // rendered. `w->GetID()` instead hashes against the CURRENT ID stack
    // top, which by the time this DockLayout (a child several levels into
    // the tree-walk dispatch) renders has one or more PushID() scopes from
    // sibling/ancestor elements pushed on top of it -- a different seed,
    // producing an unrelated, orphan dock node that gets silently discarded
    // by ImGui's per-frame housekeeping one frame after being built. Hash
    // directly against window->ID (the seed at the very top of Begin/End,
    // before anything else was pushed) to match regardless of how deep the
    // DockLayout sits in the tree.
    target_id = ImHashStr(target.c_str(), 0, w->ID);
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
