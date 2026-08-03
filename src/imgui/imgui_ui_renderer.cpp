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

#include <server/renderer.hpp>

#include <imgui.h>

#include <algorithm>
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
// dot-path (e.g. "__notepad_0") reads the same as the source that produced
// it, and is exactly as stable across runs as a hash of it would be.
std::string stable_id(const ui_element& node) {
  const auto* path_field = node.findField("__path__"_key);
  if (path_field && path_field->is<std::string>()) {
    const std::string& path = path_field->as<std::string>();
    if (!path.empty())
      return path;
  }
  return std::to_string(node.get_as<key_t>("__wish_id"_key, key_t{}).id);
}

// Append "###<stable_id>" to a Window's title. Only render_window needs
// this: ImGui::Begin() computes a top-level window's persistent ID by
// hashing its name string directly -- unlike every other widget, it does
// NOT consult the current ID stack -- so PushID() (see render_node() in
// imgui_renderer.cpp, which scopes every other widget) can't disambiguate
// windows. Three hashes (not two) is deliberate: ImGui hides everything
// after "##" from display but still folds the visible prefix into the ID
// hash, whereas "###" makes the ID depend *only* on what follows it. Using
// "##" here would let editing the Window's own title field change its
// ImGui ID out from under it, silently resetting per-ID state ImGui tracks
// itself -- position/size/dock/focus -- even though the element's actual
// identity never changed.
static std::string with_id(const std::string& label, const ui_element& node) {
  return label + "###" + stable_id(node);
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
  const_cast<ui_element&>(node)["__wish_win_rect_x__"_key] = pos.x;
  const_cast<ui_element&>(node)["__wish_win_rect_y__"_key] = pos.y;
  const_cast<ui_element&>(node)["__wish_win_rect_w__"_key] = size.x;
  const_cast<ui_element&>(node)["__wish_win_rect_h__"_key] = size.y;
}

// ── Core ──────────────────────────────────────────────────────────────────────

void render_window(imgui_renderer& r, const ui_element& node, const context& s) {
  auto title = node.get_as<std::string>("title"_key, "");
  int32_t px = node.get_as<int32_t>("pos_x"_key, -1);
  int32_t py = node.get_as<int32_t>("pos_y"_key, -1);
  int32_t w = node.get_as<int32_t>("width"_key, 0);
  int32_t h = node.get_as<int32_t>("height"_key, 0);
  int32_t fl = node.get_as<int32_t>("flags"_key, 0);
  bool closable = node.get_as<bool>("closable"_key, false);
  bool modal = node.get_as<bool>("modal"_key, false);

  // Automatically reserve menu bar space when a direct MenuBar child exists.
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    if (child.as<key_t>(dynamic::CLASS) == "MenuBar"_key)
      fl |= ImGuiWindowFlags_MenuBar;
  });

  // FirstUseEver (not Once): Once re-applies the descriptor's pos/size on
  // every process run regardless of what's saved in imgui.ini -- since
  // "the first SetNextWindowPos/Size call for this window's ID" is true
  // again on every fresh launch. FirstUseEver instead only falls back to
  // these values when the window has no persisted ini entry yet, so a
  // user's manual move/resize actually survives a restart.
  if (px >= 0 && py >= 0)
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
    // Latch "already opened" in a hidden field, same idiom as
    // __was_docked__ below, so setting modal=true is the one-shot trigger.
    const auto* opened_f = node.findField("__modal_opened__"_key);
    bool was_open = opened_f && opened_f->is<bool>() && opened_f->as<bool>();
    if (!was_open) {
      ImGui::OpenPopup(iml.c_str());
      const_cast<ui_element&>(node)["__modal_opened__"_key] = true;
    }

    bool now_open = ImGui::BeginPopupModal(iml.c_str(), p_open, ImGuiWindowFlags(fl));
    if (now_open) {
      // BeginPopupModal() returns false without calling Begin() at all when
      // the popup isn't open -- gating on now_open avoids capturing the
      // *enclosing* window's rect in that case.
      report_self_rect(node);
      render_children(r, node, s);
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
        const_cast<ui_element&>(node)["__request_close__"_key] = false;
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
      const_cast<ui_element&>(node)["__modal_opened__"_key] = false;
    }

    if (closable) {
      // Title-bar X path: identical detection to the non-modal case below.
      if (!open)
        enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "closed"_key, dynamic{});
    } else if (was_open && !now_open) {
      // No title bar to close from -- this transition means an in-content
      // handler called ImGui::CloseCurrentPopup().
      enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "closed"_key, dynamic{});
    }
    return;
  }

  bool window_open = ImGui::Begin(iml.c_str(), p_open, ImGuiWindowFlags(fl));
  // Begin()/BeginChild() are the only ImGui calls where a matching End() is
  // required regardless of the return value -- a collapsed/clipped window
  // still has a valid position/size to report, so this runs unconditionally
  // (matching the unconditional ImGui::End() below), not just when true.
  report_self_rect(node);

  if (window_open) {
    // ImGui's docking branch resizes a window to fit its dock node/tab
    // region; on undock it does not restore the pre-dock floating size
    // (docking is handled entirely inside ImGui, invisible to wish). Track
    // docked state and the last-known floating size in hidden fields (same
    // idiom as __selected__/__open__ above) and force the size back on the
    // frame the window transitions from docked to floating.
    if (!(fl & ImGuiWindowFlags_NoDocking)) {
      bool is_docked = ImGui::IsWindowDocked();
      const auto* prev_f = node.findField("__was_docked__"_key);
      bool was_docked = (prev_f && prev_f->is<bool>()) ? prev_f->as<bool>() : is_docked;

      if (was_docked && !is_docked) {
        int32_t fw = node.get_as<int32_t>("__float_width__"_key, 0);
        int32_t fh = node.get_as<int32_t>("__float_height__"_key, 0);
        if (fw > 0 && fh > 0)
          ImGui::SetWindowSize(ImVec2(float(fw), float(fh)));
      }

      if (!is_docked) {
        ImVec2 cur = ImGui::GetWindowSize();
        const_cast<ui_element&>(node)["__float_width__"_key] = int32_t(cur.x);
        const_cast<ui_element&>(node)["__float_height__"_key] = int32_t(cur.y);
      }

      const_cast<ui_element&>(node)["__was_docked__"_key] = is_docked;
    }

    render_children(r, node, s);
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
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "closed"_key, dynamic{});
}

void render_label(imgui_renderer&, const ui_element& node, const context&) {
  auto text = node.get_as<std::string>("text"_key, "");
  auto color = node.get_as<std::string>("text_color"_key, "");
  bool wrap = node.get_as<bool>("wrap"_key, false);
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

void render_button(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  int32_t w = node.get_as<int32_t>("width"_key, 0);
  int32_t h = node.get_as<int32_t>("height"_key, 0);
  if (ImGui::Button(label.c_str(), ImVec2(float(w), float(h))))
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "clicked"_key, dynamic{});
}

void render_checkbox(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  bool val = node.get_as<bool>("value"_key, false);
  if (ImGui::Checkbox(label.c_str(), &val)) {
    const_cast<ui_element&>(node)["value"_key] = val;
    dynamic payload;
    payload["value"_key] = val;
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
  }
}

void render_slider_float(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  float val = node.get_as<float>("value"_key, 0.0f);
  float vmin = node.get_as<float>("min"_key, 0.0f);
  float vmax = node.get_as<float>("max"_key, 1.0f);
  auto fmt = node.get_as<std::string>("format"_key, "%.2f");
  if (ImGui::SliderFloat(label.c_str(), &val, vmin, vmax, fmt.c_str())) {
    const_cast<ui_element&>(node)["value"_key] = val;
    dynamic payload;
    payload["value"_key] = val;
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
  }
}

void render_slider_int(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  int32_t val = node.get_as<int32_t>("value"_key, 0);
  int32_t vmin = node.get_as<int32_t>("min"_key, 0);
  int32_t vmax = node.get_as<int32_t>("max"_key, 100);
  if (ImGui::SliderInt(label.c_str(), &val, vmin, vmax)) {
    const_cast<ui_element&>(node)["value"_key] = val;
    dynamic payload;
    payload["value"_key] = val;
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
  }
}

void render_input_text(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  auto hint = node.get_as<std::string>("hint"_key, "");
  int32_t maxlen = node.get_as<int32_t>("max_length"_key, 256);
  auto current = node.get_as<std::string>("value"_key, "");
  float width = node.get_as<float>("width"_key, 0.0f);
  int32_t flags = node.get_as<int32_t>("flags"_key, 0);

  if (width != 0.0f)
    ImGui::SetNextItemWidth(width);

  std::vector<char> buf(static_cast<size_t>(maxlen) + 1, '\0');
  auto copy_len = std::min(static_cast<size_t>(maxlen), current.size());
  std::copy_n(current.c_str(), copy_len, buf.data());

  bool changed = hint.empty()
      ? ImGui::InputText(label.c_str(), buf.data(), buf.size(), ImGuiInputTextFlags(flags))
      : ImGui::InputTextWithHint(label.c_str(), hint.c_str(), buf.data(), buf.size(), ImGuiInputTextFlags(flags));

  if (changed) {
    std::string new_val(buf.data());
    const_cast<ui_element&>(node)["value"_key] = new_val;
    dynamic payload;
    payload["value"_key] = new_val;
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
  }
}

void render_image(imgui_renderer& r, const ui_element& node, const context& s) {
  auto src = node.get_as<std::string>("src"_key, "");
  int32_t w = node.get_as<int32_t>("width"_key, 0);
  int32_t h = node.get_as<int32_t>("height"_key, 0);

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
  if (node.get_as<bool>("__auto_size_to_font__"_key, false)) {
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
  auto tint = node.get_as<std::string>("tint"_key, "");
  ImVec4 tint_col = tint.empty() ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : parse_hex_color(tint);
  // Internal-only escape hatch, same idiom as "__auto_size_to_font__" above:
  // form-generated icons (file_dialog.cpp's per-row type icon) want to track
  // the *current* text color so they stay visible against both the light
  // and dark theme, and across a theme switch while the dialog is open --
  // baking a fixed "tint" hex string in at tree-construction time can't do
  // that (it's outside any ImGui frame and has no session-style access
  // either), so this reads ImGuiCol_Text fresh every render instead.
  if (node.get_as<bool>("__tint_to_text_color__"_key, false))
    tint_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  ImGui::ImageWithBg(tex, ImVec2(float(w), float(h)), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint_col);
}

void render_separator(imgui_renderer&, const ui_element&, const context&) {
  ImGui::Separator();
}

void render_separator_text(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  ImGui::SeparatorText(label.c_str());
}

void render_vertical_layout(imgui_renderer& r, const ui_element& node, const context& s) {
  float spacing = node.get_as<float>("spacing"_key, 0.0f);

  // Pre-scan children for a "height" hint (see Layout::height's field
  // comment): 0 leaves a child auto-sized exactly as before; a positive
  // value reserves fixed pixels; a negative value makes it a stretch row
  // sharing whatever space remains, weighted by magnitude. ImGui is
  // immediate-mode, so an auto child's height this frame isn't knowable
  // ahead of rendering it -- instead its *last frame's* measured height
  // (from `s.layout_height_cache`) is reserved here, and the cache entry
  // is overwritten with this frame's actual measurement after it renders.
  // This lets a fixed-height header/footer plus one stretch-filling body
  // work regardless of child order, self-correcting within one frame of
  // any auto child's height changing. A plain VerticalLayout of leaf
  // widgets (no height set on any child) takes the same code path it
  // always did, since fixed_total/stretch_weight_total stay at 0.
  struct child_info {
    ui_element* elem;
    float height;
    std::string id;
  };
  std::vector<child_info> children;
  float fixed_total = 0.0f;
  float stretch_weight_total = 0.0f;
  int n = 0;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    float h = child.get_as<float>("height"_key, 0.0f);
    std::string id = stable_id(child);
    children.push_back({&child, h, id});
    if (h > 0.0f)
      fixed_total += h;
    else if (h < 0.0f)
      stretch_weight_total += -h;
    else {
      auto it = s.layout_height_cache.find(id);
      if (it != s.layout_height_cache.end())
        fixed_total += it->second;
    }
    ++n;
  });
  float spacing_total = (n > 1) ? spacing * static_cast<float>(n - 1) : 0.0f;
  float stretch_pool = std::max(0.0f, ImGui::GetContentRegionAvail().y - fixed_total - spacing_total);

  bool first = true;
  for (auto& c : children) {
    if (!first && spacing > 0.0f)
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + spacing);
    first = false;

    if (c.height != 0.0f) {
      // Constrain the child to a dedicated child window of the computed
      // row height, so nested "fill available height" fields (Table's
      // outer_height=0/negative) resolve against this row instead of the
      // whole VerticalLayout's content region.
      float row_h = c.height > 0.0f
          ? c.height
          : (stretch_weight_total > 0.0f ? stretch_pool * (-c.height / stretch_weight_total) : 0.0f);
      auto child_id = "##vl_row_" + c.id;
      ImGui::BeginChild(child_id.c_str(), ImVec2(0.0f, row_h), ImGuiChildFlags_None);
      r.render_node(*c.elem, s);
      ImGui::EndChild();
    } else {
      float y_before = ImGui::GetCursorPosY();
      r.render_node(*c.elem, s);
      s.layout_height_cache[c.id] = ImGui::GetCursorPosY() - y_before;
    }
  }
}

void render_horizontal_layout(imgui_renderer& r, const ui_element& node, const context& s) {
  float spacing = node.get_as<float>("spacing"_key, 0.0f);
  auto align = node.get_as<std::string>("align"_key, "left");

  if (align == "right") {
    // Sum explicit child widths to compute the right-edge offset.
    float total = 0.0f;
    int n = 0;
    node.for_each_child_ordered([&](key_t, ui_element& child) {
      total += static_cast<float>(child.get_as<int32_t>("width"_key, 0));
      ++n;
    });
    if (n > 1)
      total += spacing * static_cast<float>(n - 1);
    float avail = ImGui::GetContentRegionAvail().x;
    float offset = avail - total;
    if (offset > 0.0f)
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
  }

  // Pre-scan children for a "width" hint (see Layout::width's field
  // comment): 0 leaves a child auto-sized exactly as before; a positive
  // value reserves fixed pixels; a negative value makes it a stretch
  // column sharing whatever space remains, weighted by magnitude. This
  // pass only measures -- rendering happens below -- so a plain
  // HorizontalLayout of leaf widgets (no width set on any child) takes
  // the same code path it always did.
  struct child_info {
    ui_element* elem;
    float width;
  };
  std::vector<child_info> children;
  float fixed_total = 0.0f;
  float stretch_weight_total = 0.0f;
  int n = 0;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    float w = child.get_as<float>("width"_key, 0.0f);
    children.push_back({&child, w});
    if (w > 0.0f)
      fixed_total += w;
    else if (w < 0.0f)
      stretch_weight_total += -w;
    ++n;
  });
  float spacing_total = (n > 1) ? spacing * static_cast<float>(n - 1) : 0.0f;
  float stretch_pool = std::max(0.0f, ImGui::GetContentRegionAvail().x - fixed_total - spacing_total);

  ImGui::BeginGroup();
  bool first = true;
  for (auto& c : children) {
    if (!first)
      ImGui::SameLine(0.0f, spacing);
    first = false;
    // Each child gets its own group so ImGui::SameLine() below anchors to
    // the child's whole bounding box, not just the last individual widget
    // it happened to render. Without this, a child that is itself a
    // multi-widget subtree (a nested VerticalLayout, a Table, ...) throws
    // off SameLine() for every sibling that follows it -- see DESIGN.md's
    // "HorizontalLayout row containing VerticalLayout columns" case.
    ImGui::BeginGroup();
    if (c.width != 0.0f) {
      // Constrain the child to a dedicated child window of the computed
      // column width, so nested "fill available width" fields (InputText's
      // width=-1, Table's outer_width=0/negative) resolve against this
      // column instead of the whole HorizontalLayout's content region.
      float col_w = c.width > 0.0f
          ? c.width
          : (stretch_weight_total > 0.0f ? stretch_pool * (-c.width / stretch_weight_total) : 0.0f);
      // A 0.0f height in BeginChild() means "stretch to fill the parent's
      // remaining vertical space", not "auto-size to content" -- without
      // ImGuiChildFlags_AutoResizeY every fixed-width column would balloon
      // to the full remaining window height, shoving whatever follows this
      // row far down (this broke Notepad's toolbar row: each fixed-width
      // button's child window ate the whole window height, pushing the
      // tab bar/editor below it into a sliver at the bottom).
      auto child_id = "##hl_col_" + stable_id(*c.elem);
      ImGui::BeginChild(child_id.c_str(), ImVec2(col_w, 0.0f), ImGuiChildFlags_AutoResizeY);
      r.render_node(*c.elem, s);
      ImGui::EndChild();
    } else {
      r.render_node(*c.elem, s);
    }
    ImGui::EndGroup();
  }
  ImGui::EndGroup();
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
      trailing_label = (child.as<key_t>(dynamic::CLASS) == "Label"_key) ? &child : nullptr;
    });

    node.for_each_child_ordered([&](key_t, ui_element& child) {
      if (&child != trailing_label)
        r.render_node(child, s);
    });

    if (trailing_label) {
      auto text = trailing_label->get_as<std::string>("text"_key, "");
      float text_w = ImGui::CalcTextSize(text.c_str()).x;
      float avail = ImGui::GetContentRegionAvail().x;
      if (avail > text_w)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - text_w);
      ImGui::TextUnformatted(text.c_str());
    }

    ImGui::EndMenuBar();
  }
}

void render_menu(imgui_renderer& r, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  bool enabled = node.get_as<bool>("enabled"_key, true);
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

void render_menu_item(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  auto shortcut = node.get_as<std::string>("shortcut"_key, "");
  bool checked = node.get_as<bool>("checked"_key, false);
  bool enabled = node.get_as<bool>("enabled"_key, true);
  const char* sc = shortcut.empty() ? nullptr : shortcut.c_str();
  if (ImGui::MenuItem(label.c_str(), sc, &checked, enabled)) {
    const_cast<ui_element&>(node)["checked"_key] = checked;
    dynamic payload;
    payload["checked"_key] = checked;
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "clicked"_key, std::move(payload));
  }
}

void render_menu_button(imgui_renderer& r, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
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

// ── Tabs ──────────────────────────────────────────────────────────────────────

void render_tab_bar(imgui_renderer& r, const ui_element& node, const context& s) {
  auto id = node.get_as<std::string>("id"_key, "##tabbar");
  if (ImGui::BeginTabBar(id.c_str())) {
    render_children(r, node, s);
    ImGui::EndTabBar();
  }
}

void render_tab_item(imgui_renderer& r, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "Tab");
  bool closable = node.get_as<bool>("closable"_key, false);
  bool open = true;
  bool* p_open = closable ? &open : nullptr;

  bool is_selected = ImGui::BeginTabItem(label.c_str(), p_open);

  // Emit 'selected' only on the transition from invisible to visible.
  const auto* prev_f = node.findField("__selected__"_key);
  bool was_selected = (prev_f && prev_f->is<bool>()) ? prev_f->as<bool>() : is_selected;
  const_cast<ui_element&>(node)["__selected__"_key] = is_selected;
  if (is_selected && !was_selected)
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "selected"_key, dynamic{});

  if (is_selected) {
    render_children(r, node, s);
    ImGui::EndTabItem();
  }

  if (closable && !open)
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "closed"_key, dynamic{});
}

// ── Tree ──────────────────────────────────────────────────────────────────────

void render_tree_node(imgui_renderer& r, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  bool init_open = node.get_as<bool>("open"_key, false);
  bool leaf = node.get_as<bool>("leaf"_key, false);

  ImGui::SetNextItemOpen(init_open, ImGuiCond_Once);

  ImGuiTreeNodeFlags flags =
      leaf ? (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen) : ImGuiTreeNodeFlags_None;
  bool is_open = ImGui::TreeNodeEx(label.c_str(), flags);

  const auto* prev_f = node.findField("__open__"_key);
  bool was_open = (prev_f && prev_f->is<bool>()) ? prev_f->as<bool>() : is_open;
  const_cast<ui_element&>(node)["__open__"_key] = is_open;
  if (is_open != was_open) {
    dynamic payload;
    payload["open"_key] = is_open;
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "toggled"_key, std::move(payload));
  }

  if (is_open && !leaf) {
    render_children(r, node, s);
    ImGui::TreePop();
  }
}

void render_collapsing_header(imgui_renderer& r, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  bool is_open = ImGui::CollapsingHeader(label.c_str());

  const auto* prev_f = node.findField("__open__"_key);
  bool was_open = (prev_f && prev_f->is<bool>()) ? prev_f->as<bool>() : is_open;
  const_cast<ui_element&>(node)["__open__"_key] = is_open;
  if (is_open != was_open) {
    dynamic payload;
    payload["open"_key] = is_open;
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "toggled"_key, std::move(payload));
  }

  if (is_open)
    render_children(r, node, s);
}

// ── Selection ─────────────────────────────────────────────────────────────────

void render_combo(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  auto items_str = node.get_as<std::string>("items"_key, "");
  int32_t sel = node.get_as<int32_t>("value"_key, 0);

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
    const_cast<ui_element&>(node)["value"_key] = int32_t(cur);
    dynamic payload;
    payload["value"_key] = int32_t(cur);
    if (cur >= 0 && cur < int(items.size()))
      payload["text"_key] = items[size_t(cur)];
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
  }
}

void render_radio_button(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  bool active = node.get_as<bool>("active"_key, false);
  if (ImGui::RadioButton(label.c_str(), active))
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "clicked"_key, dynamic{});
}

void render_selectable(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  bool selected = node.get_as<bool>("selected"_key, false);
  float w = node.get_as<float>("width"_key, 0.0f);
  float h = node.get_as<float>("height"_key, 0.0f);
  bool v = selected;
  if (ImGui::Selectable(label.c_str(), &v, 0, ImVec2(w, h))) {
    const_cast<ui_element&>(node)["selected"_key] = v;
    dynamic payload;
    payload["selected"_key] = v;
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
  }
}

// ── Numeric inputs ────────────────────────────────────────────────────────────

void render_input_int(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  int32_t val = node.get_as<int32_t>("value"_key, 0);
  int32_t step = node.get_as<int32_t>("step"_key, 1);
  int32_t step_fast = node.get_as<int32_t>("step_fast"_key, 100);
  int v = val;
  if (ImGui::InputInt(label.c_str(), &v, step, step_fast)) {
    const_cast<ui_element&>(node)["value"_key] = int32_t(v);
    dynamic payload;
    payload["value"_key] = int32_t(v);
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
  }
}

void render_input_float(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  float val = node.get_as<float>("value"_key, 0.0f);
  float step = node.get_as<float>("step"_key, 0.0f);
  float step_fast = node.get_as<float>("step_fast"_key, 0.0f);
  auto fmt = node.get_as<std::string>("format"_key, "%.3f");
  float v = val;
  if (ImGui::InputFloat(label.c_str(), &v, step, step_fast, fmt.c_str())) {
    const_cast<ui_element&>(node)["value"_key] = v;
    dynamic payload;
    payload["value"_key] = v;
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
  }
}

void render_drag_float(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  float val = node.get_as<float>("value"_key, 0.0f);
  float speed = node.get_as<float>("speed"_key, 1.0f);
  float vmin = node.get_as<float>("min"_key, 0.0f);
  float vmax = node.get_as<float>("max"_key, 0.0f);
  auto fmt = node.get_as<std::string>("format"_key, "%.3f");
  float v = val;
  if (ImGui::DragFloat(label.c_str(), &v, speed, vmin, vmax, fmt.c_str())) {
    const_cast<ui_element&>(node)["value"_key] = v;
    dynamic payload;
    payload["value"_key] = v;
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
  }
}

void render_drag_int(imgui_renderer&, const ui_element& node, const context& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  int32_t val = node.get_as<int32_t>("value"_key, 0);
  float speed = node.get_as<float>("speed"_key, 1.0f);
  int32_t vmin = node.get_as<int32_t>("min"_key, 0);
  int32_t vmax = node.get_as<int32_t>("max"_key, 0);
  int v = val;
  if (ImGui::DragInt(label.c_str(), &v, speed, vmin, vmax)) {
    const_cast<ui_element&>(node)["value"_key] = int32_t(v);
    dynamic payload;
    payload["value"_key] = int32_t(v);
    enqueue_event(s, node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
  }
}

// ── Status ────────────────────────────────────────────────────────────────────

void render_progress_bar(imgui_renderer&, const ui_element& node, const context&) {
  float val = node.get_as<float>("value"_key, 0.0f);
  float w = node.get_as<float>("width"_key, -1.0f);
  float h = node.get_as<float>("height"_key, 0.0f);
  auto overlay = node.get_as<std::string>("label"_key, "");
  ImGui::ProgressBar(val, ImVec2(w, h), overlay.empty() ? nullptr : overlay.c_str());
}

// ── Docking ───────────────────────────────────────────────────────────────────

void render_dockspace_viewport(imgui_renderer& r, const ui_element& node, const context& s) {
  auto id = node.get_as<std::string>("id"_key, "##viewport_dockspace");
  int32_t flags = node.get_as<int32_t>("flags"_key, 0);
  bool passthru = node.get_as<bool>("passthru"_key, false);

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
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    if (child.as<bison::key_t>(bison::dynamic::CLASS) == "MenuBar"_key)
      host_flags |= ImGuiWindowFlags_MenuBar;
  });

  ImGui::Begin(id.c_str(), nullptr, host_flags);
  report_self_rect(node);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();

  ImGuiDockNodeFlags dock_flags = ImGuiDockNodeFlags(flags);
  if (passthru)
    dock_flags |= ImGuiDockNodeFlags_PassthruCentralNode;
  ImGui::DockSpace(ImGui::GetID(id.c_str()), ImVec2(0.0f, 0.0f), dock_flags);

  // Non-Window children (e.g. MenuBar) are rendered inside the host window.
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    if (child.as<bison::key_t>(bison::dynamic::CLASS) != "Window"_key)
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
    if (child.as<bison::key_t>(bison::dynamic::CLASS) == "Window"_key)
      r.render_node(child, s);
  });
}

void render_dockspace(imgui_renderer&, const ui_element& node, const context&) {
  auto id = node.get_as<std::string>("id"_key, "dockspace");
  float width = node.get_as<float>("width"_key, 0.0f);
  float height = node.get_as<float>("height"_key, 0.0f);
  int32_t flags = node.get_as<int32_t>("flags"_key, 0);
  ImGui::DockSpace(ImGui::GetID(id.c_str()), ImVec2(width, height), ImGuiDockNodeFlags(flags));
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

void render_table(imgui_renderer& r, const ui_element& node, const context& s) {
  auto id = node.get_as<std::string>("id"_key, "##table");
  int32_t columns = node.get_as<int32_t>("columns"_key, 1);
  int32_t flags = node.get_as<int32_t>("flags"_key, 0);
  float outer_w = node.get_as<float>("outer_width"_key, 0.0f);
  float outer_h = node.get_as<float>("outer_height"_key, 0.0f);
  float inner_w = node.get_as<float>("inner_width"_key, 0.0f);
  bool headers = node.get_as<bool>("headers"_key, false);

  if (!ImGui::BeginTable(id.c_str(), columns, ImGuiTableFlags(flags), ImVec2(outer_w, outer_h), inner_w))
    return;

  const key_t table_id = node.get_as<key_t>("__wish_id"_key, key_t{});

  // Column setup must precede any row; iterate TableColumn children first.
  // column_id is passed through as ImGui's per-column user_data so a click
  // on this column's header can be mapped back to it by ColumnUserID rather
  // than by position (see the sort-spec handling below).
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    if (child.as<bison::key_t>(bison::dynamic::CLASS) != "TableColumn"_key)
      return;
    auto label = child.get_as<std::string>("label"_key, "");
    int32_t col_fl = child.get_as<int32_t>("flags"_key, 0);
    float col_w = child.get_as<float>("init_width"_key, 0.0f);
    int32_t col_id = child.get_as<int32_t>("column_id"_key, 0);
    ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags(col_fl), col_w, ImGuiID(col_id));
  });

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
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    if (child.as<key_t>(dynamic::CLASS) == "TableColumn"_key)
      return;

    if (child.as<key_t>(dynamic::CLASS) == "TableRow"_key) {
      int32_t row_flags = child.get_as<int32_t>("flags"_key, 0);
      float min_h = child.get_as<float>("min_height"_key, 0.0f);
      ImGui::TableNextRow(ImGuiTableRowFlags(row_flags), min_h);
      ImGui::TableSetColumnIndex(0);

      // Invisible selectable spanning all columns acts as the row hit-test.
      // AllowOverlap lets the cell Labels render on top without blocking input.
      char sel_id[32];
      std::snprintf(sel_id, sizeof(sel_id), "##row%d", row_idx);
      const float row_h = ImGui::GetTextLineHeightWithSpacing();
      const bool row_selected = child.get_as<bool>("selected"_key, false);
      const auto sf = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
          ImGuiSelectableFlags_AllowOverlap;
      bool sel = ImGui::Selectable(sel_id, row_selected, sf, ImVec2(0.0f, row_h));
      const bool dbl = sel && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
      if (dbl)
        sel = false; // promote to double-click only
      // Modifier keys must be read here, the same frame the click happens --
      // by the time the enqueued event is dispatched (a later frame/message),
      // ImGuiIO's keys may have moved on -- so they ride along in the payload
      // rather than being re-queried by the event's eventual handler.
      const bool click_ctrl = ImGui::GetIO().KeyCtrl;
      const bool click_shift = ImGui::GetIO().KeyShift;

      // Row-level drag-and-drop: attaches to the Selectable just drawn above
      // (the row's own top-level ImGui item) -- see handle_drag_drop()'s doc
      // comment on why this can't just rely on render_node()'s own generic
      // call, since TableRow children are never dispatched through
      // render_node() here, only their cells are (below).
      handle_drag_drop(child, s);

      // Overlay cell content on the same line as the selectable.
      // SameLine(0,0) for col 0 puts the cursor back to the selectable's
      // start position; TableNextColumn() advances for subsequent columns.
      int32_t col = 0;
      child.for_each_child_ordered([&](bison::key_t, ui_element& cell) {
        if (col == 0)
          ImGui::SameLine(0.0f, 0.0f);
        else
          ImGui::TableNextColumn();
        r.render_node(cell, s);
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
      }

      ++row_idx;
    } else {
      r.render_node(child, s);
    }
  });

  // Stick to the bottom when a scrollable table just grew a row, so a live
  // log's newest entry is always visible without the caller managing scroll
  // position. Only fires on growth (not on shrink/reset) so it never fights
  // a user who scrolled up to read older rows while the count is unchanged.
  if (table_id.id && (ImGuiTableFlags(flags) & ImGuiTableFlags_ScrollY)) {
    auto& cache = table_row_count_cache();
    auto& last_count = cache[table_id.id];
    if (row_idx > last_count)
      ImGui::SetScrollY(ImGui::GetScrollMaxY());
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

void render_table_row(imgui_renderer& r, const ui_element& node, const context& s) {
  // Fallback: used when render_table_row is called outside a render_table context.
  int32_t flags = node.get_as<int32_t>("flags"_key, 0);
  float min_height = node.get_as<float>("min_height"_key, 0.0f);
  ImGui::TableNextRow(ImGuiTableRowFlags(flags), min_height);
  int32_t col = 0;
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    ImGui::TableSetColumnIndex(col++);
    r.render_node(child, s);
  });
}

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
