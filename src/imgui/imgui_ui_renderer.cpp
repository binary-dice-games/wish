// MIT License © 2025 Binary Dice Games
/// @file imgui_ui_renderer.cpp
/// @brief ImGui render functions for wish UI elements.
///
/// Each function maps one wish element class to the corresponding ImGui call.
/// All functions share the signature:
///   void(imgui_renderer&, const ui_element&, const context&)
/// matching the render_fn typedef in imgui_renderer.cpp.
#include "imgui_ui_renderer.hpp"

#ifdef WISH_IMGUI_ENABLED

#include <context/style_service.hpp>
#include <imgui/imgui_layout.hpp>
#include <server/renderer.hpp>
#include <ui/ui_elements/window.hpp>

#include "src/rmi/shared/profiling.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

// ── ID helper ─────────────────────────────────────────────────────────────────

// Returns __path__ verbatim (not a hash of it) when available: ImGui's
// ImGui::CreateNewWindowSettings() truncates a window's saved ini section
// name at the first "###", keeping only what follows -- so whatever this
// returns for a Window IS the literal text that ends up in imgui.ini's
// "[Window][...]" line. A hash would make that file unreadable; the raw
// dot-path (e.g. "__nano_0") reads the same as the source that produced
// it, and is exactly as stable across runs as a hash of it would be.
std::string stable_id(const ui_element& node) {
  const std::string& path = node.path_ref();
  if (!path.empty())
    return path;
  key_t wish_id = node.wish_id();
  if (wish_id.id != 0)
    return std::to_string(wish_id.id);
  // Neither "__path__" nor "__wish_id" -- a form-generated node with no RMI
  // identity of its own (e.g. file_browser_utils.cpp's make_name_cell()
  // per-row icon+label HorizontalLayout: every row's instance hits this
  // branch identically). Falling back to a fixed literal ("0") here made
  // every such node share the exact same id within a single frame --
  // harmless when nothing actually keyed off it (the pre-refactor render
  // path never gave an unhinted auto Layout its own BeginChild at all), but
  // render_vertical_layout()/render_horizontal_layout() now always wrap
  // their own content in a self-managed BeginChild(id) (see
  // src/imgui/DESIGN.md's "Draw pass"), so N sibling rows sharing one
  // literal id collide on the same real ImGui child window within the same
  // frame -- confirmed live via tree's file listing: every row's
  // name-cell content silently failed to render (ImGui window-ID reuse).
  // The node's own address is stable for the node's own lifetime and, more
  // importantly, unique among every *other* node alive this same frame
  // (siblings are always distinct C++ objects) -- exactly what's needed to
  // disambiguate a per-frame widget id, even though (unlike a real
  // "__wish_id") it is not stable across a rebuild of the same logical row.
  // That's an acceptable trade-off for these nodes: they have no persisted
  // per-id ImGui state worth keeping across a rebuild anyway (no scrolling,
  // no open/collapsed state), which is exactly why they were never given a
  // stable identity of their own in the first place.
  char buf[24];
  std::snprintf(buf, sizeof(buf), "@%p", static_cast<const void*>(&node));
  return buf;
}

// Append "###<stable_id>" to a label. Needed by any widget whose persisted
// per-ID ImGui state matters AND whose visible label can change at runtime
// from a field the app controls -- e.g. a Window's title (position/size/
// dock/focus) or a TabBar's TabItem label (which tab is selected/active).
// ImGui folds a widget's whole label string into its ID hash by default, so
// without this, editing such a label silently changes the widget's ID out
// from under it -- from ImGui's point of view a brand-new widget appears in
// the old one's place, discarding whatever state was keyed to the old ID,
// even though the element's actual identity never changed. Three hashes
// (not two) is deliberate: ImGui hides everything after "##" from display
// but still folds the visible prefix into the ID hash, whereas "###" makes
// the ID depend *only* on what follows it -- required for render_window()
// specifically, since ImGui::Begin() computes a top-level window's
// persistent ID by hashing its name string directly and, unlike every other
// widget, does NOT consult the current ID stack, so PushID() (see
// render_node() in imgui_renderer.cpp, which scopes every other widget)
// can't disambiguate windows the way it can for a nested widget like
// TabItem. Using "###" for TabItem too, rather than relying on the "##"
// two-hash form, keeps both call sites identical and equally immune to a
// same-named sibling tab's label colliding with this one's stable suffix.
static std::string with_id(const std::string& label, const ui_element& node) {
  return label + "###" + stable_id(node);
}

// Builds "<prefix><stable-id-of-node>" into a reused per-thread buffer and
// returns a C-string valid until the next call on this thread. The layout
// render functions build a BeginChild id per (hinted) child per frame; this
// avoids the std::string operator+ allocations that cost. The returned
// pointer must be consumed before the next call -- every call site passes it
// straight into an ImGui::BeginChild()/PushID() that copies it immediately,
// so a later render_node() recursion reusing the buffer is harmless.
static const char* prefixed_stable_id(const char* prefix, const ui_element& node) {
  static thread_local std::string buf;
  buf.assign(prefix);
  const std::string& path = node.path_ref();
  if (!path.empty())
    buf += path;
  else
    buf += stable_id(node);
  return buf.c_str();
}

// Stamps the current ImGui window's rect onto @p node as four hidden fields
// (same idiom as __was_docked__/__float_width__/__float_height__ below),
// read back by imgui_renderer::render_node() as the authoritative rect for
// Window/DockSpaceViewport -- the only two classes that open a genuine new
// top-level ImGui window, whose content a BeginGroup()/EndGroup() wrap
// around the dispatch call can't see across. A per-node field (rather than
// a single shared slot) is required because a Window can nest inside
// another Window/DockSpaceViewport (e.g. a modal opened from within a
// docked window), which would otherwise clobber a shared slot before the
// outer container gets to read it. Only valid to call between a successful
// Begin()/BeginPopupModal() and its matching End()/EndPopup().
static void report_self_rect(const ui_element& node) {
  ImVec2 pos = ImGui::GetWindowPos();
  ImVec2 size = ImGui::GetWindowSize();
  node.set_self_rect({pos.x, pos.y}, {size.x, size.y});
}

// Companion to report_self_rect() for a node whose "own wrap" was a plain
// ImGui::BeginGroup() rather than a real BeginChild() window -- there is no
// GetWindowPos()/GetWindowSize() to query, so the caller passes the group's
// own bounding box (from GetItemRectMin/Max() right after EndGroup())
// directly. Stamps the identical "__wish_win_rect_*__" fields so
// render_node()'s self_reports_rect branch (imgui_renderer.cpp) reads
// equally accurate geometry regardless of which wrap mechanism a particular
// call used -- see render_vertical_layout()/render_horizontal_layout()'s
// suppress_layout_wrap_self handling.
static void report_self_rect_from(const ui_element& node, ImVec2 rect_min, ImVec2 rect_max) {
  node.set_self_rect({rect_min.x, rect_min.y}, {rect_max.x - rect_min.x, rect_max.y - rect_min.y});
}

// ── Core ──────────────────────────────────────────────────────────────────────

void render_window(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const window&>(node0);
  const std::string& title = node.title_ref();
  int32_t px = node.pos_x(-1);
  int32_t py = node.pos_y(-1);
  int32_t w = node.width_i(0);
  int32_t h = node.height_i(0);
  int32_t fl = node.flags(0);
  bool closable = node.closable(false);
  bool modal = node.modal(false);

  // Automatically reserve menu bar space when a direct MenuBar child exists
  // (cached on the node -- see ui_element::has_menu_bar_child()).
  if (node.has_menu_bar_child())
    fl |= ImGuiWindowFlags_MenuBar;

  // FirstUseEver (not Once): Once re-applies the descriptor's pos/size on
  // every process run regardless of what's saved in imgui.ini -- since
  // "the first SetNextWindowPos/Size call for this window's ID" is true
  // again on every fresh launch. FirstUseEver instead only falls back to
  // these values when the window has no persisted ini entry yet, so a
  // user's manual move/resize actually survives a restart.
  //
  // pos_x/pos_y default to -1 ("unpositioned"): an explicit position pins
  // the window there, while -1 opts into the auto-placement below --
  // centered (modal) or docked (normal) -- instead of ImGui's own cascade.
  bool has_explicit_pos = px >= 0 && py >= 0;
  if (has_explicit_pos)
    ImGui::SetNextWindowPos(ImVec2(float(px), float(py)), ImGuiCond_FirstUseEver);
  if (w > 0 && h > 0)
    ImGui::SetNextWindowSize(ImVec2(float(w), float(h)), ImGuiCond_FirstUseEver);

  bool open = true;
  bool* p_open = closable ? &open : nullptr;
  auto iml = with_id(title, node);

  if (modal) {
    // Docking concepts (dock nodes, floating-size restore) don't apply to a
    // popup-modal window.
    fl |= ImGuiWindowFlags_NoDocking;

    // Unlike ImGui::Begin(), a modal popup must be opened exactly once via
    // ImGui::OpenPopup() -- calling it every frame re-opens/re-centers it.
    // Latch "already opened" on the window itself (see window::modal_opened()),
    // so setting modal=true is the one-shot trigger.
    bool was_open = node.modal_opened();
    if (!was_open) {
      ImGui::OpenPopup(iml.c_str());
      node.set_modal_opened(true);
    }

    // An un-positioned modal centers on the viewport (the Win32-MessageBox
    // convention) rather than falling to ImGui's default top-left placement.
    // FirstUseEver, not Appearing: a user who drags the dialog elsewhere
    // still has that position remembered by imgui.ini across reopens, matching
    // every other Window here. An explicit pos_x/pos_y opts out.
    if (!has_explicit_pos) {
      ImGui::SetNextWindowPos(
          ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    }

    bool now_open = ImGui::BeginPopupModal(iml.c_str(), p_open, ImGuiWindowFlags(fl));
    if (now_open) {
      // Same settle-frames need as render_menu()/render_combo()/
      // render_menu_button(): opening the modal enqueues no wish event of
      // its own, so nothing else forces the couple of follow-up frames
      // ImGui's own AlwaysAutoResize sizing (or any other content whose
      // natural size comes from ui_element::last_rendered_size() rather
      // than a measure_fn formula -- see imgui_layout.cpp's
      // measure_dispatch_fns() comment -- and so needs at least one real
      // render to know its own size) may need to converge.
      // IsWindowAppearing() is true exactly the frame this popup starts
      // appearing. This alone does not fix a node that can never reach a
      // real first measurement in the first place (see
      // imgui_renderer.cpp's item_visible/had_prior_confirmed_size gating
      // for that -- the actual fix for message boxes opening too small
      // with their message text clipped off entirely), but is still a
      // real, independent gap on its own: without it, content that
      // legitimately needs 2-3 frames to settle only gets there if some
      // unrelated input event happens to keep the render loop alive long
      // enough, exactly like every other popup-opening call site here.
      if (ImGui::IsWindowAppearing())
        s.dirty.store(kDirtySettleFrames, std::memory_order_release);

      // BeginPopupModal() returns false without calling Begin() at all when
      // the popup isn't open -- gating on now_open avoids capturing the
      // *enclosing* window's rect in that case.
      report_self_rect(node);
      {
        BISON_TRACE_SCOPE("measure_node");
        measure_node(r, node, s);
      }
      {
        BISON_TRACE_SCOPE("arrange_node");
        arrange_node(r, node, ImVec2(0.0f, 0.0f), ImGui::GetContentRegionAvail(), s);
      }
      {
        BISON_TRACE_SCOPE("render_children");
        render_children(r, node, s);
      }
      // App-level code (e.g. a form's on_event()) runs outside any ImGui
      // frame and can't call ImGui::CloseCurrentPopup() directly -- it
      // requests a close by setting this hidden field instead (see
      // message_box::request_close() for a concrete example), and this is
      // the one place that actually calls it, from *inside* the Begin/End
      // scope CloseCurrentPopup() requires. Without this, app code has no way to
      // properly close a modal it opened: simply removing the Window from
      // top_level_objects (as every other close path does) leaves ImGui's
      // own internal popup stack thinking that ID is still open forever,
      // which corrupts the *next* modal that happens to reuse the same
      // stable id (e.g. a form's next instance, since
      // form::next_available_key() recycles freed keys) -- it silently
      // fails to open correctly until an unrelated input event (e.g. a
      // mouse move) forces ImGui to reconcile its popup stack.
      if (node.get_as<bool>("__request_close__"_key, false)) {
        ImGui::CloseCurrentPopup();
        const_cast<ui_element&>(node0)["__request_close__"_key] = false;
        // CloseCurrentPopup() only takes effect on ImGui's *next* pass
        // through this popup (BeginPopupModal won't return false until
        // then), and nothing else guarantees the render loop schedules that
        // next frame -- setting a hidden field here doesn't enqueue an
        // event or otherwise mark the session dirty on its own. Force it
        // directly (dirty is mutable, same idiom as the TextEditor caret
        // blink in imgui_text_editor_renderer.cpp) so the transition that
        // fires "closed" (below) actually gets observed within a frame or
        // two, instead of sitting pending until an unrelated input event
        // (e.g. a mouse move) happens to trigger the next render.
        s.dirty.store(kDirtySettleFrames, std::memory_order_release);
      }
      {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 size = ImGui::GetWindowSize();
        draw_highlight_if_set(node, pos, ImVec2(pos.x + size.x, pos.y + size.y));
      }
      ImGui::EndPopup();
    } else {
      node.set_modal_opened(false);
    }

    if (closable) {
      // Title-bar X path: identical detection to the non-modal case below.
      if (!open)
        enqueue_event(s, node.wish_id(), "closed"_key, dynamic{});
    } else if (was_open && !now_open) {
      // No title bar to close from -- this transition means an in-content
      // handler called ImGui::CloseCurrentPopup().
      enqueue_event(s, node.wish_id(), "closed"_key, dynamic{});
    }
    return;
  }

  // An un-positioned, dockable window docks into the frame's ambient
  // dockspace (the app host chrome's, or a DockSpaceViewport's) by default,
  // so a tool launched into a desktop shell opens docked instead of as a
  // loose floating window. FirstUseEver: once the user drags it out, that
  // choice is remembered by imgui.ini. An explicit pos_x/pos_y or a
  // NoDocking flag opts out; so does the absence of any dockspace this frame
  // (ambient_dockspace_id() == 0), leaving placement to ImGui.
  if (!has_explicit_pos && !(fl & ImGuiWindowFlags_NoDocking) && r.ambient_dockspace_id() != 0)
    ImGui::SetNextWindowDockID(r.ambient_dockspace_id(), ImGuiCond_FirstUseEver);

  bool window_open = ImGui::Begin(iml.c_str(), p_open, ImGuiWindowFlags(fl));
  // Begin()/BeginChild() are the only ImGui calls where a matching End() is
  // required regardless of the return value -- a collapsed/clipped window
  // still has a valid position/size to report, so this runs unconditionally
  // (matching the unconditional ImGui::End() below), not just when true.
  report_self_rect(node);

  // Detect a collapse/expand transition (title-bar arrow click) the same way
  // just_undocked() tracks docking below: the click enqueues no wish event,
  // so without an explicit settle-frame bump here the collapse re-layout can
  // sit half-applied until unrelated input (e.g. a title-bar drag) happens to
  // drive the next render -- same failure mode as render_combo()'s
  // IsWindowAppearing() check (see kDirtySettleFrames's doc comment).
  if (node.collapse_transitioned(ImGui::IsWindowCollapsed()))
    s.dirty.store(kDirtySettleFrames, std::memory_order_release);

  if (window_open) {
    // Same settle-frames need as the modal path above (see its own comment
    // for the full "AlwaysAutoResize + last_rendered_size()-fallback
    // content needs a second real frame to converge" reasoning) -- applies
    // here too since any Window, not just a modal one, can set
    // "flags":"AlwaysAutoResize".
    if (ImGui::IsWindowAppearing())
      s.dirty.store(kDirtySettleFrames, std::memory_order_release);

    // ImGui's docking branch resizes a window to fit its dock node/tab
    // region; on undock it does not restore the pre-dock floating size
    // (docking is handled entirely inside ImGui, invisible to wish). Track
    // docked state and the last-known floating size on the window itself
    // (see window::just_undocked()/set_float_size()) and force the size
    // back on the frame the window transitions from docked to floating.
    if (!(fl & ImGuiWindowFlags_NoDocking)) {
      bool is_docked = ImGui::IsWindowDocked();
      if (node.just_undocked(is_docked)) {
        int32_t fw = node.float_width();
        int32_t fh = node.float_height();
        if (fw > 0 && fh > 0)
          ImGui::SetWindowSize(ImVec2(float(fw), float(fh)));
      }

      if (!is_docked) {
        ImVec2 cur = ImGui::GetWindowSize();
        node.set_float_size(int32_t(cur.x), int32_t(cur.y));
      }
    }
    {
      BISON_TRACE_SCOPE("measure_node");
      measure_node(r, node, s);
    }
    {
      BISON_TRACE_SCOPE("arrange_node");
      arrange_node(r, node, ImVec2(0.0f, 0.0f), ImGui::GetContentRegionAvail(), s);
    }
    {
      BISON_TRACE_SCOPE("render_children");
      render_children(r, node, s);
    }
  }
  // Drawn before End() (unconditionally, same reasoning as report_self_rect()'s
  // own unconditional call above) -- imgui_renderer::render_node()'s generic
  // post-dispatch highlight code can't do this itself for Window/
  // DockSpaceViewport, since by then this End() has already run and there is
  // no longer a current window for GetWindowDrawList() to attribute to.
  {
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    draw_highlight_if_set(node, pos, ImVec2(pos.x + size.x, pos.y + size.y));
  }
  ImGui::End();

  if (closable && !open)
    enqueue_event(s, node.wish_id(), "closed"_key, dynamic{});
}

// Reads a "#RRGGBBAA"/"#RRGGBB" hex color field, preferring a
// `<field>_light`/`<field>_dark` variant over the plain field when the
// variant matching the session's *current* active theme is set. Read live
// every frame via style_service::is_light_theme() -- not cached -- so a
// theme change made after the element was created recolors it on the very
// next frame, instead of a caller baking in one color at data-creation
// time and having it go stale (the motivating case: tail's per-severity
// log line colors, set once at ingest time via tail::append_row(); see
// Label.text_color_light/text_color_dark's doc comments, label.cpp).
// @param light_key/dark_key  The `<field>_light`/`<field>_dark` keys.
// @param base_key            The plain, theme-independent field to fall
//                             back to when the matching variant is empty.
static std::string get_theme_color(const ui_element& node, const context& s, key_t base_key, key_t light_key,
    key_t dark_key) {
  bool is_light = !s.style_service || s.style_service->is_light_theme();
  auto variant = node.get_as<std::string>(is_light ? light_key : dark_key, "");
  return !variant.empty() ? variant : node.get_as<std::string>(base_key, "");
}

void render_label(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_label&>(node0);
  const std::string& text = node.text_ref();
  bool is_light = !s.style_service || s.style_service->is_light_theme();
  const std::string& color = node.text_color_ref(is_light);
  bool wrap = node.wrap(false);

  if (!color.empty())
    ImGui::PushStyleColor(ImGuiCol_Text, parse_hex_color(color));
  // GetCursorPosX() + GetContentRegionAvail().x (not PushTextWrapPos(0.0f),
  // which wraps at the *window's* right edge) so this wraps at the current
  // table column's boundary when used inside a table cell, not the whole
  // host window's edge.
  if (wrap)
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
  ImGui::TextUnformatted(text.c_str());
  if (wrap)
    ImGui::PopTextWrapPos();
  if (!color.empty())
    ImGui::PopStyleColor();
}

void render_button(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_button&>(node0);
  const std::string& label = node.label_ref();
  int32_t w = node.width_i(0);
  int32_t h = node.height_i(0);
  if (ImGui::Button(label.c_str(), ImVec2(float(w), float(h))))
    enqueue_event(s, node.wish_id(), "clicked"_key, dynamic{});
}

void render_checkbox(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_checkbox&>(node0);
  const std::string& label = node.label_ref();
  bool val = node.value_bool(false);
  if (ImGui::Checkbox(label.c_str(), &val)) {
    const_cast<ui_element&>(node0)["value"_key] = val;
    dynamic payload;
    payload["value"_key] = val;
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }
}

void render_slider_float(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_slider_float&>(node0);
  const std::string& label = node.label_ref();
  float val = node.value_float(0.0f);
  float vmin = node.min_float(0.0f);
  float vmax = node.max_float(1.0f);
  auto fmt = node.format("%.2f");
  float width = node.width(0.0f);
  if (width != 0.0f)
    ImGui::SetNextItemWidth(width);
  if (ImGui::SliderFloat(label.c_str(), &val, vmin, vmax, fmt.c_str())) {
    const_cast<ui_element&>(node0)["value"_key] = val;
    dynamic payload;
    payload["value"_key] = val;
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }
}

void render_slider_int(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_slider_int&>(node0);
  const std::string& label = node.label_ref();
  int32_t val = node.value_int(0);
  int32_t vmin = node.min_int(0);
  int32_t vmax = node.max_int(100);
  float width = node.width(0.0f);
  if (width != 0.0f)
    ImGui::SetNextItemWidth(width);
  if (ImGui::SliderInt(label.c_str(), &val, vmin, vmax)) {
    const_cast<ui_element&>(node0)["value"_key] = val;
    dynamic payload;
    payload["value"_key] = val;
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }
}

void render_input_text(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_input_text&>(node0);
  const std::string& label = node.label_ref();
  const std::string& hint = node.hint_ref();
  int32_t maxlen = node.max_length(256);
  const std::string& current = node.value_string_ref();
  float width = node.width(0.0f);
  int32_t flags = node.flags(0);
  bool multiline = node.multiline(false);
  float height = node.height(0.0f);

  if (width != 0.0f)
    ImGui::SetNextItemWidth(width);

  std::vector<char> buf(static_cast<size_t>(maxlen) + 1, '\0');
  auto copy_len = std::min(static_cast<size_t>(maxlen), current.size());
  std::copy_n(current.c_str(), copy_len, buf.data());

  bool changed;
  if (multiline) {
    // ImGui::InputTextMultiline has no hint-text overload -- "hint" is
    // silently ignored for a multiline box, matching ImGui's own API shape.
    changed = ImGui::InputTextMultiline(
        label.c_str(), buf.data(), buf.size(), ImVec2(0.0f, height), ImGuiInputTextFlags(flags));
  } else {
    changed = hint.empty()
        ? ImGui::InputText(label.c_str(), buf.data(), buf.size(), ImGuiInputTextFlags(flags))
        : ImGui::InputTextWithHint(label.c_str(), hint.c_str(), buf.data(), buf.size(), ImGuiInputTextFlags(flags));
  }

  if (changed) {
    std::string new_val(buf.data());
    const_cast<ui_element&>(node0)["value"_key] = new_val;
    dynamic payload;
    payload["value"_key] = new_val;
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }
}

void render_color_edit(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_color_edit&>(node0);
  const std::string& label = node.label_ref();
  auto value = node.value_floats();
  int32_t flags = node.flags(0);
  float width = node.width(0.0f);
  if (width != 0.0f)
    ImGui::SetNextItemWidth(width);

  // Any component count other than 3/4 is treated as 4 (padding/truncating),
  // matching this codebase's existing "malformed input is a no-op for the
  // affected part, not an error" convention (see e.g. genie's
  // text_to_floats()).
  bool use_alpha = value.size() != 3;
  std::array<float, 4> comps{1.0f, 1.0f, 1.0f, 1.0f};
  for (size_t i = 0; i < value.size() && i < comps.size(); ++i)
    comps[i] = value[i];

  bool changed = use_alpha ? ImGui::ColorEdit4(label.c_str(), comps.data(), ImGuiColorEditFlags(flags))
                            : ImGui::ColorEdit3(label.c_str(), comps.data(), ImGuiColorEditFlags(flags));

  if (changed) {
    std::vector<float> new_val(comps.begin(), comps.begin() + (use_alpha ? 4 : 3));
    const_cast<ui_element&>(node0)["value"_key] = new_val;
    dynamic payload;
    payload["value"_key] = new_val;
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }
}

void render_image(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_image&>(node0);
  const std::string& src = node.src_ref();
  int32_t w = node.width_i(0);
  int32_t h = node.height_i(0);

  // Internal-only escape hatch, not a public field on Image (see
  // image.cpp's width/height doc comments) -- lets form-generated icons
  // (e.g. file_dialog.cpp's per-row type icon) size themselves to the
  // current font's line height instead of a hardcoded pixel value, so the
  // icon reads as roughly text-height regardless of --font_size. Tree
  // construction happens outside any ImGui frame and has no way to query
  // font metrics itself -- only this render call, running inside the
  // frame, can. Deliberately left width/height at their default 0 on the
  // node itself (rather than stamping a computed pixel value in): a
  // nonzero "width" on an Image nested in a HorizontalLayout is read by
  // render_horizontal_layout() below as a column-width hint and wraps the
  // child in its own ImGui::BeginChild(), keyed by the element's
  // stable_id() -- form-generated row icons have no "__path__"/"__wish_id"
  // of their own (see stable_id()'s fallback), so every row's icon would
  // collide on the same BeginChild ID and visually merge into one.
  // Leaving "width" unset sidesteps that path entirely.
  if (node.auto_size_to_font(false)) {
    int32_t line = static_cast<int32_t>(ImGui::GetTextLineHeight());
    w = line;
    h = line;
  }

  // Every early-out below reserves the declared width/height via a Dummy
  // item instead of submitting nothing at all, so a sibling in the same
  // HorizontalLayout/VerticalLayout always sees this element occupy
  // consistent space regardless of load state (missing src, an
  // unresolvable path, or a texture that fails to decode). Submitting
  // *nothing* left an empty ImGui::BeginGroup()/EndGroup() pair (see
  // render_horizontal_layout() below), which corrupts ImGui::SameLine()'s
  // cursor math for whatever renders next -- observed as a sibling Label
  // silently clipping itself out of a modal Window entirely (see
  // message_box.cpp's icon+message row). A w<=0 or h<=0 Image has nothing
  // to reserve either way, matching prior behavior for a genuinely
  // zero-sized element.
  auto reserve = [&] {
    if (w > 0 && h > 0)
      ImGui::Dummy(ImVec2(float(w), float(h)));
  };

  if (src.empty() || w <= 0 || h <= 0) {
    reserve();
    return;
  }
  static const std::vector<std::string> kImageExtensions{"png", "jpg", "jpeg", "bmp", "gif", "webp", "tga"};
  auto full_path = file_service::resolve_or_fetch(
      src, s.resource_dir, s.allow_absolute_paths, s.allow_url_fetch, kImageExtensions);
  if (full_path.empty()) {
    reserve();
    return;
  }
  ImTextureID tex = r.get_or_load_texture(full_path.string(), s.resource_dir, &s.embedded_crc32s);
  if (!tex) {
    reserve();
    return;
  }
  auto tint = get_theme_color(node, s, "tint"_key, "tint_light"_key, "tint_dark"_key);
  ImVec4 tint_col = tint.empty() ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : parse_hex_color(tint);
  // Internal-only escape hatch, same idiom as "__auto_size_to_font__" above:
  // form-generated icons (file_dialog.cpp's per-row type icon) want to track
  // the *current* text color so they stay visible against both the light
  // and dark theme, and across a theme switch while the dialog is open --
  // baking a fixed "tint" hex string in at tree-construction time can't do
  // that (it's outside any ImGui frame and has no session-style access
  // either), so this reads ImGuiCol_Text fresh every render instead.
  if (node.tint_to_text_color(false))
    tint_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  ImGui::ImageWithBg(tex, ImVec2(float(w), float(h)), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint_col);
}

void render_separator(imgui_renderer&, const ui_element&, const context&) {
  ImGui::Separator();
}

void render_separator_text(imgui_renderer&, const ui_element& node0, const context&) {
  const auto& node = static_cast<const ui_separator_text&>(node0);
  const std::string& label = node.label_ref();
  ImGui::SeparatorText(label.c_str());
}

void render_spring(imgui_renderer&, const ui_element& node, const context&) {
  // Sized by render_horizontal_layout()/render_vertical_layout()'s arrange
  // pass (via ensure_arranged()) just before dispatching to this function --
  // {0,0} when a Spring is used outside either layout (no arrange_fn ever
  // ran for its parent), mirroring Layout.width/height's "ignored outside
  // its layout" convention.
  vec2f sz = node.arranged_size();
  ImGui::Dummy(ImVec2(sz.x, sz.y));
}

void render_vertical_layout(imgui_renderer& r, const ui_element& node, const context& s) {
  // ensure_arranged() resolves every child's size in one top-down pass
  // (self-healing against the live cursor if no ancestor Window hook
  // already did it -- see src/imgui/DESIGN.md) using each child's "height"
  // hint (see Layout::height's field comment): 0 -> the child's own
  // measured natural size, +N -> fixed pixels, -N -> a weighted share of
  // whatever space remains. A Spring child (spring.cpp's "weight" field)
  // folds its weight into the same stretch pool as a negative-height child.
  // Only each child's resolved *size* is consumed below -- position is left
  // entirely to ImGui's own natural top-to-bottom cursor advance (see the
  // loop below), which is what makes ItemSpacing/WindowPadding apply for
  // free instead of needing to be re-derived here.
  ensure_arranged(r, node, s);
  // content_extent(), not arranged_size(): the latter is "how much space
  // this node was *given*" (its own row/column allocation, or for a
  // self-healed root, whatever the ambient avail happened to be), which
  // only equals the true content size when a stretch/fill child soaks up
  // the remainder. A node with no such child (e.g. file_browser_utils.cpp's
  // per-row icon-then-label HorizontalLayout, self-healing inside a Table
  // cell where the ambient GetContentRegionAvail() is "whatever's left in
  // the whole scrollable table region", not this one row) has a true
  // content size far short of its arranged_size() -- sizing this node's own
  // BeginChild wrap to arranged_size() there would balloon this panel out
  // to that whole ambient region instead of hugging its actual content.
  vec2f self_size = node.content_extent();

  // A degenerate (<=0) self size means "don't wrap" outright (see the
  // hinted-child branch below for the identical reasoning): ImGui::
  // BeginChild() treats a 0/negative component as "fill the parent's
  // remaining space", not "auto-size to nothing", so an empty/zero-content
  // auto VerticalLayout would otherwise balloon to fill whatever's left in
  // its ambient container. Render children directly in that case (still via
  // natural flow) instead of opening a real child window.
  bool has_self_size = self_size.x > 0.0f && self_size.y > 0.0f;
  // suppress_layout_wrap_self (set by render_table() around a TableRow
  // cell's dispatch -- see that flag's doc comment in context.hpp) means
  // this node's content must stay click-transparent to the row's Selectable
  // beneath it: a real BeginChild() is a genuine ImGui window, which always
  // wins hover/click priority over whatever's behind it in the parent
  // window regardless of ImGuiSelectableFlags_AllowOverlap. There is no
  // BeginChild() flag that opts a window out of that priority while still
  // letting its own descendants receive input (ImGuiWindowFlags_NoMouseInputs
  // disables input for the whole subtree, which would also break a real
  // interactive widget placed in a cell, e.g. a Button). So this branch
  // wraps in a plain BeginGroup() instead when suppressed -- a group is not
  // a window at all, so there is nothing for it to ever intercept hover
  // from, matching a plain (unwrapped) Label cell's existing behavior.
  //
  // "scroll": children render inside their own vertically-scrolling child
  // region sized to the space this layout was *given* (arranged_size, i.e. a
  // stretch/fixed height its parent handed it), NOT to its own content
  // extent -- so overflowing content scrolls here instead of clipping or
  // ratcheting the enclosing window. Same idiom as render_tab_item()'s
  // scroll branch. A scroll layout's children should be auto/fixed height,
  // not stretch (there is no bounded remainder to stretch against).
  // Never open a real child window inside a TableRow cell (see
  // suppress_layout_wrap_self's note above) -- a scroll region there would
  // steal the row's click-through; fall back to the plain path.
  bool want_scroll = node.scroll(false) && !s.suppress_layout_wrap_self;
  bool wrap_self = has_self_size && !s.suppress_layout_wrap_self && !want_scroll;
  bool group_wrap_self = has_self_size && s.suppress_layout_wrap_self;
  if (want_scroll) {
    // Prefer the live content region (this layout renders inside whatever box
    // its parent's arrange pass already gave it); fall back to arranged_size
    // for the rare self-healed-root case where no ancestor constrained it.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float sw = avail.x > 1.0f ? avail.x : node.arranged_size().x;
    float sh = avail.y > 1.0f ? avail.y : node.arranged_size().y;
    auto child_id = "##vlscroll_" + stable_id(node);
    ImGui::BeginChild(
        child_id.c_str(), ImVec2(sw > 1.0f ? sw : 1.0f, sh > 1.0f ? sh : 1.0f), ImGuiChildFlags_None);
    report_self_rect(node);
  } else if (wrap_self) {
    // One real BeginChild for the *whole* row set, not one per hinted
    // child -- self-sizes to this node's own actual content extent and
    // reports its own rect via GetWindowPos()/GetWindowSize() (see
    // self_reports_rect in imgui_renderer.cpp), the same idiom
    // Window/DockSpaceViewport already use, replacing the old
    // BeginGroup()/trailing-Dummy() rect-capture approximation entirely.
    // ImGuiChildFlags_None (no AlwaysUseWindowPadding): a wish Layout is a
    // transparent flex-style container, not a bordered panel -- it's
    // ItemSpacing (between children) the user's original report was about,
    // not WindowPadding (a margin around the container's own edge, which
    // was never part of what was broken and would shift every child's
    // position away from this node's own reported edges).
    ImGui::BeginChild(
        prefixed_stable_id("##vl_", node), ImVec2(self_size.x, self_size.y), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    report_self_rect(node);
  } else if (group_wrap_self) {
    ImGui::BeginGroup();
  }

  // Scoped to only this row set's own direct children -- popped before
  // EndChild() below so it never leaks into this node's own gap from its
  // *next sibling* in the parent's flow (see the matching push/pop in
  // render_horizontal_layout() for the identical reasoning), and pushed
  // unconditionally (not just when "spacing" is explicitly set) so a nested
  // VerticalLayout with no override of its own doesn't inherit an outer
  // ancestor's explicit override instead of the theme default.
  float spacing = effective_spacing(node, ImGui::GetStyle().ItemSpacing.y);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, spacing));

  node.for_each_child_ordered([&](key_t, ui_element& child) {
    bool is_spring = child.class_key() == "Spring"_key;
    float height_hint = is_spring ? 0.0f : child.height(0.0f);
    vec2f size = child.arranged_size();
    // size.x/size.y <= 0 is also treated as "don't wrap", not just an auto
    // height hint -- ImGui::BeginChild() treats a 0 (or negative) component
    // as "fill the parent's remaining space on that axis" (CalcItemSize()'s
    // size==0 branch), NOT "auto-size to content" the way this code
    // originally assumed. A stretch child (height:-N) whose resolved share
    // came out to exactly 0 this frame (e.g. the stretch pool momentarily
    // exhausted) would otherwise get handed BeginChild(w, 0), which ImGui
    // silently reinterprets as "as tall as whatever's left in the ambient
    // scroll region" -- often much taller than intended.
    if (is_spring || height_hint == 0.0f || size.x <= 0.0f || size.y <= 0.0f) {
      // No BeginChild() wrap: a Spring has no content to constrain, and an
      // auto (height:0) child renders at its own natural size in ImGui's
      // normal sequential flow -- wrapping it anyway would change ImGui's
      // "last item" identity (a BeginChild counts as an item in its own
      // right), breaking anything that keys off the child's own top-level
      // item after the fact, e.g. a following ContextMenu sibling's
      // BeginPopupContextItem() attaching to "the preceding item".
      r.render_node(child, s);
      return;
    }

    // Constrain the child to a dedicated child window of the computed row
    // size, so nested "fill available height" fields (Table's
    // outer_height=0, ...) resolve against this row instead of the whole
    // VerticalLayout's content region. NoScrollbar/NoScrollWithMouse: this
    // child exists purely to pin the row to a computed size, not to be
    // independently scrollable -- any widget inside that actually needs
    // scrolling (a Table's own ScrollY, ...) already provides its own.
    // Sizes here are exact (from the measure pass), not estimated. Position
    // is wherever ImGui's own natural flow currently sits -- a BeginChild is
    // itself a normal item, so it slots into the vertical stack exactly
    // like an unwrapped child does.
    ImGui::BeginChild(
        prefixed_stable_id("##vl_row_", child), ImVec2(size.x, size.y), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    r.render_node(child, s);
    ImGui::EndChild();
  });

  ImGui::PopStyleVar();
  if (want_scroll || wrap_self) {
    ImGui::EndChild();
  } else if (group_wrap_self) {
    // Same trailing zero-size Dummy() before EndGroup() as
    // imgui_renderer.cpp's needs_group_wrap idiom, and for the identical
    // reason: guarantees EndGroup() closes over something drawn at this
    // node's own current position rather than falling back to whatever
    // unrelated item rendered immediately before this node started (ImGui's
    // own #7543 EndTable() workaround, which EndGroup()'s bounding-box
    // computation shares).
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndGroup();
    report_self_rect_from(node, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  }
}

void render_horizontal_layout(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_horizontal_layout&>(node0);
  auto align = node.align("left");

  if (align == "right") {
    // Sum explicit child widths to compute the right-edge offset, then
    // shift the starting cursor before ensure_arranged() below captures it
    // as this row's origin -- so the whole arrange pass runs against the
    // already-shifted content region, same as the pre-refactor version.
    // Must match arrange_horizontal_layout()'s own "spacing" fallback (see
    // imgui_layout.cpp's effective_spacing()) or this offset and the
    // arrange pass it precedes would disagree about the row's total width.
    float spacing_field = node.spacing(0.0f);
    float spacing = spacing_field > 0.0f ? spacing_field : ImGui::GetStyle().ItemSpacing.x;
    float total = 0.0f;
    int n = 0;
    node.for_each_child_ordered([&](key_t, ui_element& child) {
      total += static_cast<float>(child.width_i(0));
      ++n;
    });
    if (n > 1)
      total += spacing * static_cast<float>(n - 1);
    float avail = ImGui::GetContentRegionAvail().x;
    float offset = avail - total;
    if (offset > 0.0f)
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
  }

  // ensure_arranged() resolves every child's size in one top-down pass
  // using each child's "width" hint (see Layout::width's field comment):
  // 0 -> the child's own measured natural size, +N -> fixed pixels, -N ->
  // a weighted share of whatever space remains. Each child's own "height"
  // hint is honored independently (0 -> natural, +N -> fixed, -N -> fill
  // the row) -- see arrange_horizontal_layout() in imgui_layout.cpp. A
  // Spring child (spring.cpp's "weight" field) folds its weight into the
  // same width stretch pool as a negative-width child. Only each child's
  // resolved *size* is consumed below -- horizontal position comes from
  // ImGui::SameLine() between children, same as any plain sequential ImGui
  // code, which is what makes ItemSpacing/WindowPadding apply for free.
  ensure_arranged(r, node, s);
  // See render_vertical_layout()'s identical comment: content_extent(), not
  // arranged_size(), so a self-healed row with no stretch child (e.g. a
  // table-cell icon+label row) sizes its own panel to its real content, not
  // to a possibly-huge ambient avail.
  vec2f self_size = node.content_extent();

  // See render_vertical_layout()'s identical guard for the full reasoning:
  // a degenerate self size must not be handed to BeginChild() (0/negative
  // means "fill remaining space" to ImGui, not "auto-size to nothing").
  bool has_self_size = self_size.x > 0.0f && self_size.y > 0.0f;
  // See render_vertical_layout()'s identical guard: suppress_layout_wrap_self
  // (set by render_table() around a TableRow cell's dispatch) means this
  // node's content must stay click-transparent to the row's Selectable
  // beneath it, which a real BeginChild() window can never be -- so this
  // wraps in a plain BeginGroup() instead when suppressed, the same as
  // render_vertical_layout().
  bool wrap_self = has_self_size && !s.suppress_layout_wrap_self;
  bool group_wrap_self = has_self_size && s.suppress_layout_wrap_self;
  if (wrap_self) {
    // One real BeginChild for the *whole* column set, not one per hinted
    // child -- see render_vertical_layout()'s identical comment for why
    // this replaces the old BeginGroup()/trailing-Dummy() rect-capture
    // approximation (and why it deliberately does not use
    // AlwaysUseWindowPadding).
    ImGui::BeginChild(
        prefixed_stable_id("##hl_", node), ImVec2(self_size.x, self_size.y), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    report_self_rect(node);
  } else if (group_wrap_self) {
    ImGui::BeginGroup();
  }

  // Scoped to only this column set's own direct children -- see
  // render_vertical_layout()'s identical comment for why this is pushed
  // unconditionally and popped before EndChild().
  float spacing = effective_spacing(node, ImGui::GetStyle().ItemSpacing.x);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, ImGui::GetStyle().ItemSpacing.y));

  bool first = true;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    if (!first)
      ImGui::SameLine();
    first = false;

    // Each child gets its own group so a container child (a nested
    // VerticalLayout, a Table, ...) reports one coherent bounding box to
    // imgui_renderer::render_node()'s own rect-capture wrap -- see
    // DESIGN.md's "HorizontalLayout row containing VerticalLayout columns"
    // case.
    ImGui::BeginGroup();
    bool is_spring = child.class_key() == "Spring"_key;
    float width_hint = is_spring ? 0.0f : child.width(0.0f);
    vec2f size = child.arranged_size();
    // size.x/size.y <= 0 also forces "don't wrap" -- see
    // render_vertical_layout()'s identical guard for the full reasoning.
    // This is the branch that actually hit it in practice: a column's own
    // "width" hint is nonzero (entering this branch) while its "height" is
    // left auto (0, the common case), so size.y falls through to this
    // child's own measured_size().y -- for an unregistered leaf (e.g.
    // SliderFloat), that's the last_rendered_size() fallback, which is 0 on
    // this node's very first frame or right after a prior frame's render
    // got inflated by this exact bug. A 0 component always means "fill the
    // parent's remaining space on that axis" to ImGui's own
    // BeginChildEx()/CalcItemSize(), never "auto-size to content".
    if (is_spring || width_hint == 0.0f || size.x <= 0.0f || size.y <= 0.0f) {
      // No BeginChild() wrap: a Spring has no content to constrain, and an
      // auto (width:0) child renders at its own natural size in ImGui's
      // normal sequential flow -- wrapping it anyway would change ImGui's
      // "last item" identity (a BeginChild counts as an item in its own
      // right), breaking anything that keys off the child's own top-level
      // item after the fact (see render_vertical_layout()'s identical
      // reasoning above).
      r.render_node(child, s);
    } else {
      // Constrain the child to a dedicated child window of the computed
      // column size, so nested "fill available width/height" fields
      // (InputText's width=-1, Table's outer_width/height=0, ...) resolve
      // against this column instead of the whole HorizontalLayout's
      // content region. NoScrollbar/NoScrollWithMouse for the same reason
      // as render_vertical_layout()'s row wrap: a purely structural
      // size-constraint container is not a scroll target. Sizes here are
      // exact (from the measure pass), not estimated -- this is what
      // structurally prevents the historical nano toolbar regression
      // (fixed-width buttons ballooning to the full window height).
      ImGui::BeginChild(
          prefixed_stable_id("##hl_col_", child), ImVec2(size.x, size.y), ImGuiChildFlags_None,
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      r.render_node(child, s);
      ImGui::EndChild();
    }
    ImGui::EndGroup();
  });

  ImGui::PopStyleVar();
  if (wrap_self) {
    ImGui::EndChild();
  } else if (group_wrap_self) {
    // See render_vertical_layout()'s identical trailing Dummy()/EndGroup()
    // pair for why this is needed.
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndGroup();
    report_self_rect_from(node, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  }
}

// Splitter -- imgui.com/issues/319's resizable-panes technique, implemented
// with the public InvisibleButton API rather than imgui_internal.h's
// SplitterBehavior() (no wish renderer file includes imgui_internal.h, and
// this repo avoids doing so -- see DESIGN.md).
//
// All panes but the last carry an explicit pixel size in their own "width"
// (vertical orientation) or "height" (horizontal orientation) field --
// exactly the field HorizontalLayout/VerticalLayout already read as a
// column/row-width hint on ANY child (see render_horizontal_layout()'s own
// doc comment above), so a Splitter pane's size is visible/settable the
// same way any other Layout child's is. The last pane is never stored: it
// always fills whatever space remains after the others and the bars'
// thickness, mirroring the original imgui.com/issues/319 demo (which
// likewise only ever persists size1 and recomputes size2 = avail - size1
// every frame). A drag bar between pane i and pane i+1 adjusts both by
// equal and opposite deltas, clamped so neither shrinks below
// "min_pane_size" -- when pane i+1 is the last (unstored) pane, its
// "current size" for clamping purposes is simply the remainder,
// recomputed fresh each time. Composability over an N-pane primitive:
// nest a second Splitter inside the first's last pane for 3+ panes,
// exactly like the referenced issue's own 3-pane demo calls Splitter()
// twice.
void render_splitter(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_splitter&>(node0);
  // Splitter's own outer box, so a Splitter that is itself an auto/stretch
  // child of a VerticalLayout/HorizontalLayout gets a resolved rect
  // (introspectable via automation, same as every other layout-aware
  // class) -- everything below still reads the live ambient
  // GetContentRegionAvail(), unchanged, since the wrapping BeginChild an
  // enclosing Layout already places this Splitter in already constrains
  // that correctly on its own.
  ensure_arranged(r, node, s);

  auto orientation = node.orientation("vertical");
  bool is_vertical = orientation != "horizontal";
  float thickness = std::max(1.0f, node.thickness(4.0f));
  float min_pane = std::max(0.0f, node.min_pane_size(20.0f));
  key_t size_field = is_vertical ? "width"_key : "height"_key;

  struct pane_info {
    ui_element* elem;
    float size;
  };
  std::vector<pane_info> panes;
  panes.reserve(node.resolved_child_count());
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    panes.push_back({&child, is_vertical ? child.width(0.0f) : child.height(0.0f)});
  });
  if (panes.empty())
    return;
  if (panes.size() == 1) {
    r.render_node(*panes[0].elem, s);
    return;
  }

  size_t n = panes.size();
  float avail = is_vertical ? ImGui::GetContentRegionAvail().x : ImGui::GetContentRegionAvail().y;
  float usable = std::max(0.0f, avail - thickness * static_cast<float>(n - 1));

  // First render: seed any unset (<= 0) explicit pane with an even split,
  // same "0 means unset/auto" convention as Layout::width/height. A pane
  // pre-set in the JSON descriptor (nonzero) keeps its authored size.
  if (node.consume_init_guard()) {
    float even = usable / static_cast<float>(n);
    for (size_t i = 0; i + 1 < n; ++i)
      if (panes[i].size <= 0.0f)
        panes[i].size = even;
  }
  for (size_t i = 0; i + 1 < n; ++i)
    panes[i].size = std::max(panes[i].size, min_pane);

  auto explicit_sum = [&] {
    float sum = 0.0f;
    for (size_t i = 0; i + 1 < n; ++i)
      sum += panes[i].size;
    return sum;
  };

  ImGuiMouseCursor cursor = is_vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS;
  key_t id = node.wish_id();
  int released_bar = -1;

  ImGui::BeginGroup();
  for (size_t i = 0; i < n; ++i) {
    if (i + 1 == n)
      panes[i].size = std::max(min_pane, usable - explicit_sum());

    if (i > 0 && is_vertical)
      ImGui::SameLine(0.0f, 0.0f);

    // Same idiom as render_horizontal_layout()'s "##hl_col_" + stable_id(...)
    // column-child id: disambiguates the literal-repeated BeginChild/
    // InvisibleButton labels below across panes using each pane's own
    // stable identity rather than a positional index, so ImGui's persisted
    // per-id state (scroll position, ...) survives a child being reordered.
    auto pane_sid = stable_id(*panes[i].elem);
    auto pane_child_id = "##sp_pane_" + pane_sid;
    ImVec2 child_size = is_vertical ? ImVec2(panes[i].size, 0.0f) : ImVec2(0.0f, panes[i].size);
    ImGui::BeginChild(pane_child_id.c_str(), child_size, ImGuiChildFlags_None);
    r.render_node(*panes[i].elem, s);
    ImGui::EndChild();
    ImVec2 pane_extent = ImGui::GetItemRectSize();

    if (i + 1 < n) {
      if (is_vertical)
        ImGui::SameLine(0.0f, 0.0f);
      auto bar_id = "##sp_bar_" + pane_sid;
      ImVec2 bar_size = is_vertical ? ImVec2(thickness, pane_extent.y) : ImVec2(pane_extent.x, thickness);
      ImGui::InvisibleButton(bar_id.c_str(), bar_size);
      bool bar_hovered = ImGui::IsItemHovered();
      bool bar_active = ImGui::IsItemActive();
      // InvisibleButton draws nothing on its own -- paint the bar using the
      // theme's own separator colors (brightening on hover/drag) so there is
      // a visible affordance for where to grab it, not just a cursor change.
      ImU32 bar_color = ImGui::GetColorU32(
          bar_active ? ImGuiCol_SeparatorActive : (bar_hovered ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator));
      ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), bar_color);
      if (bar_hovered || bar_active)
        ImGui::SetMouseCursor(cursor);
      if (bar_active) {
        float delta = is_vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        float lo = min_pane - panes[i].size;
        float hi = (i + 2 < n) ? panes[i + 1].size - min_pane
                                : std::max(min_pane, usable - explicit_sum()) - min_pane;
        delta = std::clamp(delta, lo, hi);
        panes[i].size += delta;
        if (i + 2 < n)
          panes[i + 1].size -= delta;
      }
      if (ImGui::IsItemDeactivated())
        released_bar = static_cast<int>(i);
    }
  }
  ImGui::EndGroup();

  // Persist every pane's final size, including the last (unstored) one --
  // so a client get() reflects the effective layout, and a nested widget
  // that itself reads "width"/"height" (SliderFloat, InputText, ...) picks
  // up its containing pane's size. A pane whose "width"/"height" isn't a
  // schema-declared field (e.g. a plain Label, unlike a Layout subclass)
  // gets its field's type from whatever the JSON literal looked like --
  // an integer literal like `"width": 150` creates an int32_t field, not
  // a float one -- and `field::operator=` enforces a stable type per
  // field, throwing if a later frame writes a different alternative.
  // Match the field's existing type instead of assuming float.
  for (auto& p : panes) {
    auto* f = p.elem->findField(size_field);
    if (f && f->is<int32_t>())
      (*p.elem)[size_field] = static_cast<int32_t>(std::lround(p.size));
    else
      (*p.elem)[size_field] = p.size;
  }

  if (released_bar >= 0) {
    dynamic payload;
    payload["pane_index"_key] = static_cast<int32_t>(released_bar);
    payload["size1"_key] = panes[static_cast<size_t>(released_bar)].size;
    payload["size2"_key] = panes[static_cast<size_t>(released_bar) + 1].size;
    enqueue_event(s, id, "resized"_key, std::move(payload));
  }
}

// ── Menu ──────────────────────────────────────────────────────────────────────

void render_menu_bar(imgui_renderer& r, const ui_element& node, const context& s) {
  if (ImGui::BeginMenuBar()) {
    // A trailing Label child (e.g. a clock) is right-aligned instead of
    // flowing left-to-right with the Menu children; anything else renders
    // in the usual menu-bar order (Menu/MenuItem stack horizontally on
    // their own, no explicit SameLine needed).
    ui_element* trailing_label = nullptr;
    node.for_each_child_ordered([&](key_t, ui_element& child) {
      trailing_label = (child.class_key() == "Label"_key) ? &child : nullptr;
    });

    node.for_each_child_ordered([&](key_t, ui_element& child) {
      if (&child != trailing_label)
        r.render_node(child, s);
    });

    if (trailing_label) {
      const std::string& text = static_cast<const ui_label*>(trailing_label)->text_ref();
      float text_w = ImGui::CalcTextSize(text.c_str()).x;
      float avail = ImGui::GetContentRegionAvail().x;
      if (avail > text_w)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - text_w);
      ImGui::TextUnformatted(text.c_str());
    }

    ImGui::EndMenuBar();
  }
}

void render_menu(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_menu&>(node0);
  const std::string& label = node.label_ref();
  bool enabled = node.enabled(true);
  if (ImGui::BeginMenu(label.c_str(), enabled)) {
    // Same settle-frames need as render_combo()/render_menu_button(): the
    // submenu popup enqueues no wish event of its own, so nothing else
    // forces the couple of follow-up frames ImGui needs to size/render its
    // newly opened content. IsWindowAppearing() is true exactly the frame
    // this popup window starts appearing.
    if (ImGui::IsWindowAppearing())
      s.dirty.store(kDirtySettleFrames, std::memory_order_release);
    render_children(r, node, s);
    ImGui::EndMenu();
  }
}

void render_menu_item(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_menu_item&>(node0);
  const std::string& label = node.label_ref();
  const std::string& shortcut = node.shortcut_ref();
  bool checked = node.checked(false);
  bool enabled = node.enabled(true);
  const char* sc = shortcut.empty() ? nullptr : shortcut.c_str();
  // `checked` is passed BY VALUE (the ImGuiSelectableFlags-less overload),
  // not `&checked` -- the pointer overload has ImGui auto-toggle *checked
  // itself* on every click, turning every plain action item (Properties,
  // Rename, Copy Path, ...) into a checkbox-style toggle with no way to
  // opt out. Passing by value still draws the check mark when true, but
  // leaves the field entirely under the form's own control (e.g. a
  // radio-style priority submenu that recomputes "checked" from server
  // state on every update, as top.cpp's row context menu does).
  if (ImGui::MenuItem(label.c_str(), sc, checked, enabled)) {
    // Clipboard access needs an active ImGui context, so this must happen
    // here on the render thread -- not in some later on_event() handler
    // over on the dispatch thread, which has no ImGui context of its own.
    const std::string& copy_text = node.copy_text_ref();
    if (!copy_text.empty())
      ImGui::SetClipboardText(copy_text.c_str());
    dynamic payload;
    payload["checked"_key] = checked;
    enqueue_event(s, node.wish_id(), "clicked"_key, std::move(payload));
  }
}

void render_menu_button(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_menu_button&>(node0);
  const std::string& label = node.label_ref();
  // Same with_id() suffix used for both the trigger Button's own ImGui id
  // and the popup's id (mirrors render_window()'s "iml" reuse for a modal's
  // Begin/OpenPopup/BeginPopupModal calls) -- a single stable identifier is
  // all either call needs, and ImGui::Button()'s widget-id namespace and
  // ImGui::OpenPopup()'s popup-id namespace don't collide even when given
  // the identical string.
  auto iml = with_id(label, node);
  if (ImGui::Button(iml.c_str())) {
    ImGui::OpenPopup(iml.c_str());
    // Same settle-frames need as render_combo() above: opening the popup
    // enqueues no wish event, so nothing else forces the couple of
    // follow-up frames ImGui needs to size/render its newly opened content.
    s.dirty.store(kDirtySettleFrames, std::memory_order_release);
  }
  if (ImGui::BeginPopup(iml.c_str())) {
    render_children(r, node, s);
    ImGui::EndPopup();
  }
}

void render_context_menu(imgui_renderer& r, const ui_element& node, const context& s) {
  // BeginPopupContextItem(NULL) both detects the right-click (on the last
  // ImGui item drawn -- normally the previous sibling in this node's own
  // parent, or the row Selectable when render_table() invokes this out of
  // its usual child-iteration order -- see ContextMenu's registration
  // comment in src/ui/ui_elements/menu.cpp) and opens/tracks the popup, so
  // unlike render_menu_button() there is no separate OpenPopup() call.
  if (ImGui::BeginPopupContextItem()) {
    // Same settle-frames need as render_menu()/render_menu_button(): the
    // popup enqueues no wish event of its own to force the couple of
    // follow-up frames ImGui needs to size/render its newly opened content.
    if (ImGui::IsWindowAppearing())
      s.dirty.store(kDirtySettleFrames, std::memory_order_release);
    render_children(r, node, s);
    ImGui::EndPopup();
  }
}

// ── Tabs ──────────────────────────────────────────────────────────────────────

void render_tab_bar(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_tab_bar&>(node0);
  const std::string& id_stored = node.id_ref();
  if (ImGui::BeginTabBar(id_stored.empty() ? "##tabbar" : id_stored.c_str())) {
    render_children(r, node, s);
    ImGui::EndTabBar();
  }
}

void render_tab_item(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_tab_item&>(node0);
  const std::string& label = node.label_ref();
  bool closable = node.closable(false);
  bool open = true;
  bool* p_open = closable ? &open : nullptr;

  // with_id(), not a raw label: BeginTabItem() folds its whole label string
  // into the tab's persistent ImGui ID (same as ImGui::Begin() -- see
  // with_id()'s doc comment above). A TabItem's label can change at runtime
  // (e.g. nano's unsaved-changes " *" suffix, toggled by editing/saving),
  // and without this, that content change silently changes the tab's ID too
  // -- from ImGui's point of view a brand-new tab appears in the old one's
  // place, which loses its active/selected status and, if it happened to be
  // the active tab, hands "active" to a neighboring tab instead.
  auto iml = label.empty() ? with_id("Tab", node) : with_id(label, node);
  bool is_selected = ImGui::BeginTabItem(iml.c_str(), p_open);

  // Emit 'selected' only on the transition from invisible to visible.
  if (node.toggled_since_last_frame(is_selected) && is_selected)
    enqueue_event(s, node.wish_id(), "selected"_key, dynamic{});

  if (is_selected) {
    // scroll: give the tab page its own BeginChild() scroll region so
    // overflowing content scrolls inside the tab instead of scrolling the
    // enclosing window (which would carry the tab-selection row out of
    // view). Any sibling widgets meant to stay pinned below the TabBar must
    // live in a different window -- the region runs to the enclosing
    // window's content bottom (documented on the field).
    //
    // Height is GetContentRegionAvail().y (scroll-invariant, per ImGui's
    // Begin() math: the -Scroll.y terms in the content-region bottom and the
    // cursor position cancel) minus one WindowPadding.y. A plain
    // BeginChild(0, 0) fill overruns the enclosing window's content bottom by
    // exactly that padding, leaving the window itself with a sliver of
    // permanent scroll range (enough to let the tab row drift); reserving it
    // keeps the window's own ScrollMax at zero. The subtraction is a small
    // fixed amount, so the child is never starved even if avail is briefly
    // odd during a dock/resize transition.
    if (node.scroll(false)) {
      auto child_id = "##tabscroll_" + stable_id(node);
      float child_h = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().WindowPadding.y;
      ImGui::BeginChild(
          child_id.c_str(), ImVec2(0, child_h > 1.0f ? child_h : 1.0f), ImGuiChildFlags_None);
      render_children(r, node, s);
      ImGui::EndChild();
    } else {
      render_children(r, node, s);
    }
    ImGui::EndTabItem();
  }

  if (closable && !open)
    enqueue_event(s, node.wish_id(), "closed"_key, dynamic{});
}

// ── Tree ──────────────────────────────────────────────────────────────────────

void render_tree_node(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_tree_node&>(node0);
  const std::string& label = node.label_ref();
  bool init_open = node.open(false);
  bool leaf = node.leaf(false);

  ImGui::SetNextItemOpen(init_open, ImGuiCond_Once);

  ImGuiTreeNodeFlags flags =
      leaf ? (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen) : ImGuiTreeNodeFlags_None;
  bool is_open = ImGui::TreeNodeEx(label.c_str(), flags);

  if (node.toggled_since_last_frame(is_open)) {
    dynamic payload;
    payload["open"_key] = is_open;
    enqueue_event(s, node.wish_id(), "toggled"_key, std::move(payload));
  }

  if (is_open && !leaf) {
    render_children(r, node, s);
    ImGui::TreePop();
  }
}

void render_collapsing_header(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_collapsing_header&>(node0);
  const std::string& label = node.label_ref();

  // Server-authoritative open state: force it every frame so ImGui never owns
  // it. A freshly-rebuilt header (a new ImGui ID each rebuild, or an ID
  // colliding with a just-freed sibling's stale storage) would otherwise
  // report a spurious open/close flip on nearly every frame; any caller that
  // rebuilds its subtree in response to "toggled" (e.g. genie's Inspector)
  // then re-triggers itself without bound. Forcing is_open to match the wish
  // field means toggled_since_last_frame() sees no change on a fresh node's
  // first frame -- no spurious event, no loop.
  bool want_open = node.open(true);
  ImGui::SetNextItemOpen(want_open, ImGuiCond_Always);
  bool is_open = ImGui::CollapsingHeader(label.c_str());

  if (node.toggled_since_last_frame(is_open)) {
    dynamic payload;
    payload["open"_key] = is_open;
    enqueue_event(s, node.wish_id(), "toggled"_key, std::move(payload));
    // The click enqueues an event, so the render loop keeps going, but a
    // consumer that rebuilds its tree in the handler needs a couple of
    // settle frames for the new content to converge (same reason
    // render_combo() bumps this when a popup opens).
    s.dirty.store(kDirtySettleFrames, std::memory_order_release);
  }

  if (is_open)
    render_children(r, node, s);
}

// ── Selection ─────────────────────────────────────────────────────────────────

void render_combo(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_combo&>(node0);
  const std::string& label = node.label_ref();
  const std::string& items_str = node.items_ref();
  int32_t sel = node.value_int(0);
  float width = node.width(0.0f);
  if (width != 0.0f)
    ImGui::SetNextItemWidth(width);

  // Build per-frame vectors from the newline-separated items string.
  std::vector<std::string> items;
  std::string::size_type pos = 0, end;
  while ((end = items_str.find('\n', pos)) != std::string::npos) {
    items.push_back(items_str.substr(pos, end - pos));
    pos = end + 1;
  }
  if (!items_str.empty())
    items.push_back(items_str.substr(pos));

  int cur = sel;
  bool changed = false;
  const char* preview = (cur >= 0 && cur < int(items.size())) ? items[size_t(cur)].c_str() : "";
  // BeginCombo()/EndCombo() (not the Combo() convenience wrapper) so the
  // popup-opening transition can be observed directly below -- same idiom
  // render_menu() uses for BeginMenu()'s submenu popup.
  if (ImGui::BeginCombo(label.c_str(), preview)) {
    // Opening the dropdown (clicking the combo header) enqueues no wish
    // event of its own -- unlike a selection change below, there is nothing
    // for the render loop's "events not empty" check (server::render_loop())
    // to see, so the popup's follow-up settle frames (see
    // kDirtySettleFrames's doc comment: ImGui auto-fit sizing can take a
    // couple of frames) never get scheduled. Without this, the newly opened
    // option list can sit invisible until an unrelated input event (e.g. a
    // mouse move) happens to drive the next render. IsWindowAppearing() is
    // true exactly the frame this popup window starts appearing.
    if (ImGui::IsWindowAppearing())
      s.dirty.store(kDirtySettleFrames, std::memory_order_release);
    for (int i = 0; i < int(items.size()); ++i) {
      bool is_selected = (cur == i);
      if (ImGui::Selectable(items[size_t(i)].c_str(), is_selected)) {
        cur = i;
        changed = true;
      }
      if (is_selected)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  if (changed) {
    const_cast<ui_element&>(node0)["value"_key] = int32_t(cur);
    dynamic payload;
    payload["value"_key] = int32_t(cur);
    if (cur >= 0 && cur < int(items.size()))
      payload["text"_key] = items[size_t(cur)];
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }
}

void render_radio_button(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_radio_button&>(node0);
  const std::string& label = node.label_ref();
  bool active = node.active(false);
  if (ImGui::RadioButton(label.c_str(), active))
    enqueue_event(s, node.wish_id(), "clicked"_key, dynamic{});
}

void render_selectable(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_selectable&>(node0);
  const std::string& label = node.label_ref();
  bool selected = node.selected(false);
  float w = node.width(0.0f);
  float h = node.height(0.0f);

  // Children (if any) are drawn as overlay content on top of the
  // Selectable's own hit-test area -- the same AllowOverlap technique
  // render_table() already uses for a TableRow's row-spanning Selectable --
  // so a Selectable can wrap richer content (e.g. an Image + a caption
  // Label) while staying clickable across all of it, not just wherever
  // Selectable's own text label happens to be drawn. Author an explicit
  // nonzero width/height when using this: 0 falls back to ImGui's plain
  // fill-width/single-line sizing, which won't cover taller overlay content.
  bool has_children = node.has_children();

  bool v = selected;
  ImGuiSelectableFlags flags = has_children ? ImGuiSelectableFlags_AllowOverlap : 0;
  ImVec2 top_left = ImGui::GetCursorScreenPos();
  if (ImGui::Selectable(label.c_str(), &v, flags, ImVec2(w, h))) {
    const_cast<ui_element&>(node0)["selected"_key] = v;
    dynamic payload;
    payload["selected"_key] = v;
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }

  if (has_children) {
    ImVec2 box_size = ImGui::GetItemRectSize();
    ImGui::SetCursorScreenPos(top_left);
    node.for_each_child_ordered([&](key_t, ui_element& child) { r.render_node(child, s); });
    // Advance the cursor to the Selectable's own bottom edge, not wherever
    // the last overlay child's natural flow left it -- otherwise a caller
    // relying on sequential stacking (e.g. render_vertical_layout()) would
    // see this cell consume more vertical space than its declared height.
    ImGui::SetCursorScreenPos(ImVec2(top_left.x, top_left.y + box_size.y));
  }
}

// ── Numeric inputs ────────────────────────────────────────────────────────────

void render_input_int(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_input_int&>(node0);
  const std::string& label = node.label_ref();
  int32_t val = node.value_int(0);
  int32_t step = node.step_int(1);
  int32_t step_fast = node.step_fast_int(100);
  int32_t flags = node.flags(0);
  float width = node.width(0.0f);
  if (width != 0.0f)
    ImGui::SetNextItemWidth(width);
  int v = val;
  if (ImGui::InputInt(label.c_str(), &v, step, step_fast, ImGuiInputTextFlags(flags))) {
    const_cast<ui_element&>(node0)["value"_key] = int32_t(v);
    dynamic payload;
    payload["value"_key] = int32_t(v);
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }
}

void render_input_float(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_input_float&>(node0);
  const std::string& label = node.label_ref();
  float val = node.value_float(0.0f);
  float step = node.step_float(0.0f);
  float step_fast = node.step_fast_float(0.0f);
  auto fmt = node.format("%.3f");
  float width = node.width(0.0f);
  if (width != 0.0f)
    ImGui::SetNextItemWidth(width);
  float v = val;
  if (ImGui::InputFloat(label.c_str(), &v, step, step_fast, fmt.c_str())) {
    const_cast<ui_element&>(node0)["value"_key] = v;
    dynamic payload;
    payload["value"_key] = v;
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }
}

void render_drag_float(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_drag_float&>(node0);
  const std::string& label = node.label_ref();
  float val = node.value_float(0.0f);
  float speed = node.speed(1.0f);
  float vmin = node.min_float(0.0f);
  float vmax = node.max_float(0.0f);
  auto fmt = node.format("%.3f");
  float v = val;
  if (ImGui::DragFloat(label.c_str(), &v, speed, vmin, vmax, fmt.c_str())) {
    const_cast<ui_element&>(node0)["value"_key] = v;
    dynamic payload;
    payload["value"_key] = v;
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }
}

void render_drag_int(imgui_renderer&, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_drag_int&>(node0);
  const std::string& label = node.label_ref();
  int32_t val = node.value_int(0);
  float speed = node.speed(1.0f);
  int32_t vmin = node.min_int(0);
  int32_t vmax = node.max_int(0);
  int v = val;
  if (ImGui::DragInt(label.c_str(), &v, speed, vmin, vmax)) {
    const_cast<ui_element&>(node0)["value"_key] = int32_t(v);
    dynamic payload;
    payload["value"_key] = int32_t(v);
    enqueue_event(s, node.wish_id(), "changed"_key, std::move(payload));
  }
}

// ── Status ────────────────────────────────────────────────────────────────────

void render_progress_bar(imgui_renderer&, const ui_element& node0, const context&) {
  const auto& node = static_cast<const ui_progress_bar&>(node0);
  float val = node.value_float(0.0f);
  float w = node.width(-1.0f);
  float h = node.height(0.0f);
  const std::string& overlay = node.label_ref();
  ImGui::ProgressBar(val, ImVec2(w, h), overlay.empty() ? nullptr : overlay.c_str());
}

// ── Docking ───────────────────────────────────────────────────────────────────

void render_dockspace_viewport(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_dockspace_viewport&>(node0);
  const std::string& id_stored = node.id_ref();
  const char* id = id_stored.empty() ? "##viewport_dockspace" : id_stored.c_str();
  int32_t flags = node.flags(0);
  bool passthru = node.passthru(false);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  // Host window fills the whole viewport, so its background reads as the
  // app's canvas rather than a widget surface -- use the theme's dedicated
  // "empty docking node" color (ImGuiCol_DockingEmptyBg) instead of
  // ImGuiCol_WindowBg, which the dark preset sets close to black.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg]);

  // Reserve menu bar space if any direct child is a MenuBar.
  ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoNavFocus;
  if (node.has_menu_bar_child())
    host_flags |= ImGuiWindowFlags_MenuBar;

  ImGui::Begin(id, nullptr, host_flags);
  report_self_rect(node);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();

  ImGuiDockNodeFlags dock_flags = ImGuiDockNodeFlags(flags);
  if (passthru)
    dock_flags |= ImGuiDockNodeFlags_PassthruCentralNode;
  // GetID() hashes against the current window, so capture the id inside this
  // host-window scope (it is no longer current once End() runs below, when
  // the Window children actually render).
  ImGuiID dockspace_id = ImGui::GetID(id);
  ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dock_flags);
  // Un-positioned Window children dock here by default (see render_window).
  r.set_ambient_dockspace_id(dockspace_id);

  // Non-Window children (e.g. MenuBar) are rendered inside the host window.
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    if (child.class_key() != "Window"_key)
      r.render_node(child, s);
  });

  // Drawn before End() -- see the identical reasoning in render_window().
  {
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    draw_highlight_if_set(node, pos, ImVec2(pos.x + size.x, pos.y + size.y));
  }

  // Host window MUST be closed before Window children are rendered so that
  // ImGui does not try to nest independent top-level windows.
  ImGui::End();

  // Window children are rendered at the top level — they will dock into
  // the dockspace created above.
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    if (child.class_key() == "Window"_key)
      r.render_node(child, s);
  });
}

void render_dockspace(imgui_renderer&, const ui_element& node0, const context&) {
  const auto& node = static_cast<const ui_dockspace&>(node0);
  const std::string& id_stored = node.id_ref();
  float width = node.width(0.0f);
  float height = node.height(0.0f);
  int32_t flags = node.flags(0);
  ImGui::DockSpace(
      ImGui::GetID(id_stored.empty() ? "dockspace" : id_stored.c_str()), ImVec2(width, height),
      ImGuiDockNodeFlags(flags));
}

// ── Table elements ────────────────────────────────────────────────────────────

// Per-widget last-seen row count, so a scrollable table (ImGuiTableFlags_
// ScrollY) can detect "a new row just appeared" and stick to the bottom --
// e.g. a live event/log table should always show its most recent entry
// without the caller having to manage scroll position itself.
static std::unordered_map<uint32_t, int32_t>& table_row_count_cache() {
  static std::unordered_map<uint32_t, int32_t> cache;
  return cache;
}

// Row index a Shift+drag range-select gesture (see render_table()'s
// "drag_extend" check below) last extended to, per table -- lets that check
// emit a row_selected only when the hovered row actually changes, instead
// of flooding one event per frame for the whole duration of the drag. -1 ==
// no drag in progress for this table. Keyed the same way as
// table_row_count_cache() above.
static std::unordered_map<uint32_t, int32_t>& table_drag_select_cache() {
  static std::unordered_map<uint32_t, int32_t> cache;
  return cache;
}

void render_table(imgui_renderer& r, const ui_element& node0, const context& s) {
  const auto& node = static_cast<const ui_table&>(node0);
  const std::string& id_stored = node.id_ref();
  const char* id = id_stored.empty() ? "##table" : id_stored.c_str();
  int32_t columns = node.columns(1);
  int32_t flags = node.flags(0);
  float outer_w = node.outer_width(0.0f);
  float outer_h = node.outer_height(0.0f);
  float inner_w = node.inner_width(0.0f);
  bool headers = node.headers(false);

  // outer_width/outer_height of exactly 0 keeps ImGui's own "auto-size to
  // content" default UNLESS an enclosing VerticalLayout/HorizontalLayout
  // gave this Table a *real* hint-driven box -- i.e. this Table's own
  // Layout "width"/"height" hint field (the field its parent reads to
  // decide fixed/stretch/auto sizing, same convention as any other Layout
  // child) is explicitly nonzero, meaning the parent actually intends to
  // constrain it (a stretch row/column, or an explicit fixed size), not
  // merely pass through this Table's own natural size unchanged.
  //
  // This is deliberately narrower than "ensure_arranged() returned true"
  // (i.e. an ancestor's arrange pass touched this node at all this frame):
  // arrange_vertical_layout()/arrange_horizontal_layout() stamp an arranged
  // rect for *every* child, hinted or not, including a plain auto
  // (height:0) Table -- for that case the "arranged size" they hand back is
  // just this Table's own last-measured natural size, not a real externally
  // imposed constraint. Filling from it anyway would create a real feedback
  // loop specific to Table (the one class whose own render call can read its
  // own measured/arranged size back into its own rendering): on an
  // outer_width/outer_height == 0 axis, Table's own measure_fn
  // (measure_table() in imgui_layout.cpp) reports last_rendered_size() --
  // this node's own *previous frame's real render* -- for the same
  // "genuine auto-size-to-content" reason the generic fallback used to.
  // If that previous render's outer_height was itself derived from
  // arranged_size() (this exact fill logic), every frame's real rendered
  // height would embed whatever ImGui adds on top of a requested
  // outer_height (a few pixels of overhead), compounding without bound --
  // confirmed via WISH_LAYOUT_DEBUG_LOG against top's auto-height
  // "proc_table" as a sustained +4px-per-frame growth, never settling.
  // Gating on the Table's own hint being nonzero breaks the cycle: a plain
  // auto Table passes outer_height=0 straight to ImGui::BeginTable() every
  // frame, unconditionally, with nothing carried over from the previous
  // frame to compound. (A negative outer_width/outer_height is a separate,
  // already-{0,0}-guarded axis in measure_table() -- see that function's
  // comment -- so this gate only needs to worry about the ==0 case.)
  float own_width_hint = node.width(0.0f);
  float own_height_hint = node.height(0.0f);
  {
    BISON_TRACE_SCOPE("table::ensure_arranged");
    ensure_arranged(r, node, s);
    if (own_width_hint != 0.0f && outer_w == 0.0f)
      outer_w = node.arranged_size().x;
    if (own_height_hint != 0.0f && outer_h == 0.0f)
      outer_h = node.arranged_size().y;
  }
  
  if (!ImGui::BeginTable(id, columns, ImGuiTableFlags(flags), ImVec2(outer_w, outer_h), inner_w))
    return;

  const key_t table_id = node.wish_id();

  // Shift+drag range-select support (see the per-row "drag_extend" check
  // below): reset this table's last-extended-to row once the mouse button
  // is released, so the next Shift+drag starts clean instead of skipping
  // its first row because it happens to match a value left over from a
  // previous drag.
  if (table_id.id && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    table_drag_select_cache()[table_id.id] = -1;

  // Column setup must precede any row; iterate TableColumn children first.
  // column_id is passed through as ImGui's per-column user_data so a click
  // on this column's header can be mapped back to it by ColumnUserID rather
  // than by position (see the sort-spec handling below).
  node.for_each_child_ordered([&](bison::key_t, ui_element& child0) {
    if (child0.class_key() != "TableColumn"_key)
      return;
    const auto& child = static_cast<const ui_table_column&>(child0);
    const std::string& label = child.label_ref();
    int32_t col_fl = child.flags(0);
    float col_w = child.init_width(0.0f);
    int32_t col_id = child.column_id(0);
    ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags(col_fl), col_w, ImGuiID(col_id));
  });

  // Pin the header row to the top of the scroll region instead of letting it
  // scroll away with the body -- ImGui requires TableSetupScrollFreeze() to
  // be called (after column setup, before the first row) once per table to
  // opt into this; it only takes effect when ScrollY is also set, since a
  // non-scrolling table has nothing for the header to stay fixed against.
  if (headers && (ImGuiTableFlags(flags) & ImGuiTableFlags_ScrollY))
    ImGui::TableSetupScrollFreeze(0, 1);

  if (headers)
    ImGui::TableHeadersRow();

  // ImGuiTableFlags_Sortable tables report header clicks via sort specs
  // rather than a normal widget event; SpecsDirty is true once when the
  // table is first created (reflecting whichever column has DefaultSort)
  // and again each time the user clicks a header. We surface it as a
  // "sorted" event and immediately clear the dirty flag ourselves --
  // ImGui does not track "already consumed", so leaving it set would
  // re-emit the same event every frame.
  if (table_id.id) {
    if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
      if (sort_specs->SpecsDirty && sort_specs->SpecsCount > 0) {
        const auto& spec = sort_specs->Specs[0];
        dynamic sort_payload;
        sort_payload["column_id"_key] = static_cast<int32_t>(spec.ColumnUserID);
        sort_payload["ascending"_key] = spec.SortDirection == ImGuiSortDirection_Ascending;
        enqueue_event(s, table_id, "sorted"_key, std::move(sort_payload));
        sort_specs->SpecsDirty = false;
      }
    }
  }

  int32_t row_idx = 0;
  key_t pending_event{};
  int32_t pending_index = -1;
  bool pending_ctrl = false;
  bool pending_shift = false;

  // Render non-column children in declaration order.  TableRow children get
  // an invisible spanning Selectable so single- and double-clicks on any cell
  // emit row_selected / row_activated on the table's wish_id.
  {
    BISON_TRACE_SCOPE("table::render_rows");
    node.for_each_child_ordered([&](bison::key_t, ui_element& child0) {
      if (child0.class_key() == "TableColumn"_key)
        return;

      if (child0.class_key() == "TableRow"_key) {
        const auto& child = static_cast<const ui_table_row&>(child0);
        // TableRow children are rendered inline rather than through the
        // generic per-element dispatch (imgui_renderer::render_node()), which
        // is where Element's own "visible" field is normally enforced -- so
        // it's checked explicitly here instead. A hidden row is skipped
        // entirely (no TableNextRow, no cells, no row_idx bump) rather than
        // rendered blank, so e.g. a log table's regex filter can hide
        // already-buffered rows without disturbing the visible rows' own
        // click-index numbering.
        if (!child.visible())
          return;

        int32_t row_flags = child.flags(0);
        float min_h = child.min_height(0.0f);
        ImGui::TableNextRow(ImGuiTableRowFlags(row_flags), min_h);
        ImGui::TableSetColumnIndex(0);

        // Invisible selectable spanning all columns acts as the row hit-test.
        // AllowOverlap lets the cell Labels render on top without blocking input.
        char sel_id[32];
        std::snprintf(sel_id, sizeof(sel_id), "##row%d", row_idx);
        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        const bool row_selected = child.selected(false);
        const auto sf = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
            ImGuiSelectableFlags_AllowOverlap;
        bool sel = ImGui::Selectable(sel_id, row_selected, sf, ImVec2(0.0f, row_h));
        // TableRow is rendered inline (see this loop's own doc comment above)
        // rather than via render_node(), so it never reaches that function's
        // generic automation hit-test capture -- ask for it explicitly here,
        // right after the row's own item, so `get_tree()`/`get_widget()` can
        // address a row directly (and so it's included among a click's
        // eligible targets for browser-driven automation).
        r.capture_hit_test_for_last_item(child);
        const bool dbl = sel && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (dbl)
          sel = false; // promote to double-click only
        // Modifier keys must be read here, the same frame the click happens --
        // by the time the enqueued event is dispatched (a later frame/message),
        // ImGuiIO's keys may have moved on -- so they ride along in the payload
        // rather than being re-queried by the event's eventual handler.
        const bool click_ctrl = ImGui::GetIO().KeyCtrl;
        const bool click_shift = ImGui::GetIO().KeyShift;

        // Shift+drag range-select: sweeping the cursor across other rows
        // while Shift stays held and the left button remains down (pressed on
        // an earlier plain click elsewhere in this table) continues to extend
        // the selection the same way a fresh Shift+click on each newly
        // hovered row would -- the "group selection" drag gesture. Gated on
        // Shift specifically so a plain click-drag stays free for this row's
        // own drag-and-drop (drag_type/drop_type, handled by
        // handle_drag_drop() right below); deduped via
        // table_drag_select_cache() so hovering the same row across several
        // frames only emits once.
        bool drag_extend = false;
        if (!sel && !dbl && click_shift && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemHovered()) {
          int32_t& last_drag_row = table_drag_select_cache()[table_id.id];
          if (last_drag_row != row_idx) {
            last_drag_row = row_idx;
            drag_extend = true;
          }
        }

        // Row-level drag-and-drop: attaches to the Selectable just drawn above
        // (the row's own top-level ImGui item) -- see handle_drag_drop()'s doc
        // comment on why this can't just rely on render_node()'s own generic
        // call, since TableRow children are never dispatched through
        // render_node() here, only their cells are (below).
        handle_drag_drop(child, s);

        // Row-level tooltip, same rationale as the handle_drag_drop() call
        // above: TableRow is never dispatched through render_node(), so its
        // own "tooltip" field would otherwise never be checked.
        handle_tooltip(child);

        // A ContextMenu child is excluded from column layout (see the loop
        // below) and rendered here instead, right after the Selectable, so
        // ImGui::BeginPopupContextItem() (inside render_context_menu) attaches
        // to the row's own hit-test item -- a cell's Label/etc. content has no
        // stable ImGui item id of its own for it to fall back to. At most one
        // ContextMenu child per row is supported; a second one is ignored.
        ui_element* context_menu = nullptr;
        child.for_each_child_ordered([&](bison::key_t, ui_element& cell) {
          if (!context_menu && cell.class_key() == "ContextMenu"_key)
            context_menu = &cell;
        });
        if (context_menu)
          r.render_node(*context_menu, s);

        // Overlay cell content on the same line as the selectable.
        // SameLine(0,0) for col 0 puts the cursor back to the selectable's
        // start position; TableNextColumn() advances for subsequent columns.
        // suppress_layout_wrap_self is set for the duration of each cell's
        // dispatch so a HorizontalLayout/VerticalLayout cell (e.g.
        // file_browser_utils.cpp's icon+label row) doesn't open its own
        // input-capturing BeginChild() over the row's Selectable above --
        // see context::suppress_layout_wrap_self's doc comment.
        int32_t col = 0;
        child.for_each_child_ordered([&](bison::key_t, ui_element& cell) {
          if (cell.class_key() == "ContextMenu"_key)
            return;
          if (col == 0)
            ImGui::SameLine(0.0f, 0.0f);
          else
            ImGui::TableNextColumn();
          s.suppress_layout_wrap_self = true;
          r.render_node(cell, s);
          s.suppress_layout_wrap_self = false;
          ++col;
        });

        // Record at most one event per frame; the last clicked row wins.
        if (dbl) {
          pending_event = "row_activated"_key;
          pending_index = row_idx;
          pending_ctrl = click_ctrl;
          pending_shift = click_shift;
        } else if (sel && !pending_event.id) {
          pending_event = "row_selected"_key;
          pending_index = row_idx;
          pending_ctrl = click_ctrl;
          pending_shift = click_shift;
        } else if (drag_extend && !pending_event.id) {
          pending_event = "row_selected"_key;
          pending_index = row_idx;
          pending_ctrl = false;
          pending_shift = true;
        }

        ++row_idx;
      } else {
        r.render_node(child0, s);
      }
    });
  }

  // Stick to the bottom when a scrollable table just grew a row, so a live
  // log's newest entry is always visible without the caller managing scroll
  // position. Only fires on growth (not on shrink/reset) so it never fights
  // a user who scrolled up to read older rows while the count is unchanged.
  // Gated on auto_scroll (default true) so a caller can offer a "Follow"
  // toggle (e.g. modules/bdg/desktop/tail) that stops the pull-to-bottom
  // without stopping row growth -- last_count is still updated either way,
  // so re-enabling auto_scroll only snaps to bottom on the *next* new row,
  // not immediately on toggle.
  //
  // SetScrollHereY(1.0f), not SetScrollY(GetScrollMaxY()): ScrollMax is the
  // content height ImGui finished computing at the *previous* frame's
  // End()/EndChild(), so it doesn't yet know about any row(s) just rendered
  // this frame. That's invisible one row at a time (each new frame's stale
  // target still lands one row short of a target that itself moves down
  // next frame, so it settles within a frame or two) but breaks visibly for
  // a batch of several rows landing in one push_lines() call/frame -- the
  // view lands wherever the bottom was *before* that whole batch. Calling
  // SetScrollHereY() right here instead targets the cursor's current Y,
  // which already reflects every row drawn this frame, batch or not (this
  // is Dear ImGui's own documented pattern for stateless bottom-follow --
  // see imgui_demo.cpp's ShowExampleAppLog/ShowExampleAppConsole).
  if (table_id.id && (ImGuiTableFlags(flags) & ImGuiTableFlags_ScrollY)) {
    auto& cache = table_row_count_cache();
    auto& last_count = cache[table_id.id];
    bool auto_scroll = node.auto_scroll(true);
    if (auto_scroll && row_idx > last_count)
      ImGui::SetScrollHereY(1.0f);
    last_count = row_idx;
  }

  ImGui::EndTable();

  if (table_id.id && pending_event.id) {
    dynamic payload;
    payload["index"_key] = pending_index;
    payload["ctrl"_key] = pending_ctrl;
    payload["shift"_key] = pending_shift;
    enqueue_event(s, table_id, pending_event, std::move(payload));
  }
}

void render_table_column(imgui_renderer&, const ui_element&, const context&) {
  // Handled inline by render_table during column setup; no-op when standalone.
}

void render_table_row(imgui_renderer& r, const ui_element& node0, const context& s) {
  // Fallback: used when render_table_row is called outside a render_table context.
  const auto& node = static_cast<const ui_table_row&>(node0);
  int32_t flags = node.flags(0);
  float min_height = node.min_height(0.0f);
  ImGui::TableNextRow(ImGuiTableRowFlags(flags), min_height);
  int32_t col = 0;
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    ImGui::TableSetColumnIndex(col++);
    r.render_node(child, s);
  });
}

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
