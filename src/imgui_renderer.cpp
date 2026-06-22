// MIT License © 2025 Binary Dice Games
/// @file imgui_renderer.cpp
/// @brief Dear ImGui concrete renderer — leaf element dispatch.
#include <wish/imgui_renderer.hpp>
#include <wish/renderer.hpp>
#include <wish/session.hpp>
#include <wish/style_service.hpp>

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_common.hpp"

#include <imgui.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Returns the RMI object ID stamped by apply_descriptor, or a fallback key.
static key_t node_id(const ui_element& node) {
  const auto* f = node.findField("__wish_id"_key);
  return (f && f->is<key_t>()) ? f->as<key_t>() : key_t{};
}

// ── Field helpers ─────────────────────────────────────────────────────────────

static std::string str_field(
    const dynamic& obj, key_t k, const char* dflt = "") {
  const auto* f = obj.findField(k);
  return (f && f->is<std::string>()) ? f->as<std::string>() : dflt;
}

static float float_field(const dynamic& obj, key_t k, float dflt = 0.0f) {
  const auto* f = obj.findField(k);
  return (f && f->is<float>()) ? f->as<float>() : dflt;
}

static int32_t int_field(const dynamic& obj, key_t k, int32_t dflt = 0) {
  const auto* f = obj.findField(k);
  return (f && f->is<int32_t>()) ? f->as<int32_t>() : dflt;
}

static bool bool_field(const dynamic& obj, key_t k, bool dflt = false) {
  const auto* f = obj.findField(k);
  return (f && f->is<bool>()) ? f->as<bool>() : dflt;
}

// ── Per-type renderers ────────────────────────────────────────────────────────

static void render_window(
    imgui_renderer& r, const ui_element& node, session& s) {
  auto title  = str_field(node, "title"_key, "");
  int32_t px  = int_field(node, "pos_x"_key, -1);
  int32_t py  = int_field(node, "pos_y"_key, -1);
  int32_t w   = int_field(node, "width"_key, 0);
  int32_t h   = int_field(node, "height"_key, 0);
  int32_t fl  = int_field(node, "flags"_key, 0);

  if (px >= 0 && py >= 0)
    ImGui::SetNextWindowPos(ImVec2(float(px), float(py)), ImGuiCond_Once);
  if (w > 0 && h > 0)
    ImGui::SetNextWindowSize(ImVec2(float(w), float(h)), ImGuiCond_Once);

  if (ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags(fl)))
    render_children(r, node, s);
  ImGui::End();
}

static void render_label(const ui_element& node) {
  auto text = str_field(node, "text"_key, "");
  ImGui::TextUnformatted(text.c_str());
}

static void render_button(const ui_element& node, session& s) {
  auto    label = str_field(node, "label"_key, "");
  int32_t w     = int_field(node, "width"_key,  0);
  int32_t h     = int_field(node, "height"_key, 0);
  if (ImGui::Button(label.c_str(), ImVec2(float(w), float(h))) && s.emit_event) {
    dynamic payload;
    s.emit_event(node_id(node), "clicked"_key, std::move(payload));
  }
}

static void render_checkbox(const ui_element& node, session& s) {
  auto label = str_field(node, "label"_key, "");
  bool val   = bool_field(node, "value"_key, false);
  if (ImGui::Checkbox(label.c_str(), &val)) {
    const_cast<ui_element&>(node)["value"_key] = val;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = val;
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

static void render_slider_float(const ui_element& node, session& s) {
  auto  label  = str_field(node, "label"_key, "");
  float val    = float_field(node, "value"_key, 0.0f);
  float vmin   = float_field(node, "min"_key, 0.0f);
  float vmax   = float_field(node, "max"_key, 1.0f);
  auto  fmt    = str_field(node, "format"_key, "%.2f");
  if (ImGui::SliderFloat(label.c_str(), &val, vmin, vmax, fmt.c_str())) {
    const_cast<ui_element&>(node)["value"_key] = val;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = val;
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

static void render_slider_int(const ui_element& node, session& s) {
  auto    label = str_field(node, "label"_key, "");
  int32_t val   = int_field(node, "value"_key, 0);
  int32_t vmin  = int_field(node, "min"_key, 0);
  int32_t vmax  = int_field(node, "max"_key, 100);
  if (ImGui::SliderInt(label.c_str(), &val, vmin, vmax)) {
    const_cast<ui_element&>(node)["value"_key] = val;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = val;
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

static void render_input_text(const ui_element& node, session& s) {
  auto    label   = str_field(node, "label"_key, "");
  auto    hint    = str_field(node, "hint"_key, "");
  int32_t maxlen  = int_field(node, "max_length"_key, 256);
  auto    current = str_field(node, "value"_key, "");

  std::vector<char> buf(static_cast<size_t>(maxlen) + 1, '\0');
  auto copy_len = std::min(static_cast<size_t>(maxlen), current.size());
  std::copy_n(current.c_str(), copy_len, buf.data());

  bool changed = hint.empty()
      ? ImGui::InputText(label.c_str(), buf.data(), buf.size())
      : ImGui::InputTextWithHint(
            label.c_str(), hint.c_str(), buf.data(), buf.size());

  if (changed) {
    std::string new_val(buf.data());
    const_cast<ui_element&>(node)["value"_key] = new_val;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = new_val;
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

static void render_vertical_layout(
    imgui_renderer& r, const ui_element& node, session& s) {
  float spacing = float_field(node, "spacing"_key, 0.0f);
  bool first = true;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    if (!first && spacing > 0.0f)
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + spacing);
    first = false;
    r.render_node(child, s);
  });
}

static void render_horizontal_layout(
    imgui_renderer& r, const ui_element& node, session& s) {
  float spacing = float_field(node, "spacing"_key, 0.0f);
  ImGui::BeginGroup();
  bool first = true;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    if (!first) ImGui::SameLine(0.0f, spacing);
    first = false;
    r.render_node(child, s);
  });
  ImGui::EndGroup();
}

static void render_image(
    imgui_renderer& r, const ui_element& node, session& s) {
  auto    src = str_field(node, "src"_key, "");
  int32_t w   = int_field(node, "width"_key, 0);
  int32_t h   = int_field(node, "height"_key, 0);
  if (src.empty() || w <= 0 || h <= 0) return;
  ImTextureID tex = r.get_or_load_texture(src, s.resource_dir);
  if (!tex) return;
  ImGui::Image(tex, ImVec2(float(w), float(h)));
}

// ── Per-session style helpers ─────────────────────────────────────────────────

// Parse "#RRGGBBAA" or "#RRGGBB" hex color string into an ImVec4.
static ImVec4 parse_hex_color(const std::string& s) {
  if (s.size() < 7 || s[0] != '#') return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  try {
    std::string hex = s.substr(1);
    if (hex.size() == 6) hex += "FF";
    if (hex.size() != 8) return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    unsigned long val = std::stoul(hex, nullptr, 16);
    return ImVec4(
        static_cast<float>((val >> 24) & 0xFF) / 255.0f,
        static_cast<float>((val >> 16) & 0xFF) / 255.0f,
        static_cast<float>((val >>  8) & 0xFF) / 255.0f,
        static_cast<float>((val      ) & 0xFF) / 255.0f);
  } catch (...) {
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  }
}

// Apply all fields from sd into style.
static void apply_style_fields(const bison::dynamic& sd, ImGuiStyle& style) {
  // Apply named preset first so per-field overrides can refine it.
  const auto* preset_f = sd.findField("preset"_key);
  if (preset_f && preset_f->is<std::string>()) {
    const std::string& p = preset_f->as<std::string>();
    if      (p == "dark")    ImGui::StyleColorsDark(&style);
    else if (p == "light")   ImGui::StyleColorsLight(&style);
    else if (p == "classic") ImGui::StyleColorsClassic(&style);
  }

  // Scalar float overrides.
  auto fset = [&](key_t k, float& target) {
    const auto* f = sd.findField(k);
    if (f && f->is<float>()) target = f->as<float>();
  };
  fset("alpha"_key,                    style.Alpha);
  fset("disabled_alpha"_key,           style.DisabledAlpha);
  fset("window_rounding"_key,          style.WindowRounding);
  fset("window_border_size"_key,       style.WindowBorderSize);
  fset("child_rounding"_key,           style.ChildRounding);
  fset("child_border_size"_key,        style.ChildBorderSize);
  fset("popup_rounding"_key,           style.PopupRounding);
  fset("popup_border_size"_key,        style.PopupBorderSize);
  fset("frame_rounding"_key,           style.FrameRounding);
  fset("frame_border_size"_key,        style.FrameBorderSize);
  fset("indent_spacing"_key,           style.IndentSpacing);
  fset("scrollbar_size"_key,           style.ScrollbarSize);
  fset("scrollbar_rounding"_key,       style.ScrollbarRounding);
  fset("grab_min_size"_key,            style.GrabMinSize);
  fset("grab_rounding"_key,            style.GrabRounding);
  fset("tab_rounding"_key,             style.TabRounding);
  fset("tab_border_size"_key,          style.TabBorderSize);
  fset("separator_text_border_size"_key, style.SeparatorTextBorderSize);

  // Vec2 overrides (stored as _x / _y float pairs).
  fset("window_padding_x"_key,       style.WindowPadding.x);
  fset("window_padding_y"_key,       style.WindowPadding.y);
  fset("frame_padding_x"_key,        style.FramePadding.x);
  fset("frame_padding_y"_key,        style.FramePadding.y);
  fset("item_spacing_x"_key,         style.ItemSpacing.x);
  fset("item_spacing_y"_key,         style.ItemSpacing.y);
  fset("item_inner_spacing_x"_key,   style.ItemInnerSpacing.x);
  fset("item_inner_spacing_y"_key,   style.ItemInnerSpacing.y);
  fset("cell_padding_x"_key,         style.CellPadding.x);
  fset("cell_padding_y"_key,         style.CellPadding.y);
  fset("button_text_align_x"_key,    style.ButtonTextAlign.x);
  fset("button_text_align_y"_key,    style.ButtonTextAlign.y);

  // Color overrides (#RRGGBBAA hex strings).
  auto cset = [&](key_t k, ImVec4& target) {
    const auto* f = sd.findField(k);
    if (f && f->is<std::string>()) target = parse_hex_color(f->as<std::string>());
  };
  cset("color_text"_key,                   style.Colors[ImGuiCol_Text]);
  cset("color_text_disabled"_key,          style.Colors[ImGuiCol_TextDisabled]);
  cset("color_window_bg"_key,              style.Colors[ImGuiCol_WindowBg]);
  cset("color_child_bg"_key,               style.Colors[ImGuiCol_ChildBg]);
  cset("color_popup_bg"_key,               style.Colors[ImGuiCol_PopupBg]);
  cset("color_border"_key,                 style.Colors[ImGuiCol_Border]);
  cset("color_border_shadow"_key,          style.Colors[ImGuiCol_BorderShadow]);
  cset("color_frame_bg"_key,               style.Colors[ImGuiCol_FrameBg]);
  cset("color_frame_bg_hovered"_key,       style.Colors[ImGuiCol_FrameBgHovered]);
  cset("color_frame_bg_active"_key,        style.Colors[ImGuiCol_FrameBgActive]);
  cset("color_title_bg"_key,               style.Colors[ImGuiCol_TitleBg]);
  cset("color_title_bg_active"_key,        style.Colors[ImGuiCol_TitleBgActive]);
  cset("color_title_bg_collapsed"_key,     style.Colors[ImGuiCol_TitleBgCollapsed]);
  cset("color_menu_bar_bg"_key,            style.Colors[ImGuiCol_MenuBarBg]);
  cset("color_scrollbar_bg"_key,           style.Colors[ImGuiCol_ScrollbarBg]);
  cset("color_scrollbar_grab"_key,         style.Colors[ImGuiCol_ScrollbarGrab]);
  cset("color_scrollbar_grab_hovered"_key, style.Colors[ImGuiCol_ScrollbarGrabHovered]);
  cset("color_scrollbar_grab_active"_key,  style.Colors[ImGuiCol_ScrollbarGrabActive]);
  cset("color_check_mark"_key,             style.Colors[ImGuiCol_CheckMark]);
  cset("color_slider_grab"_key,            style.Colors[ImGuiCol_SliderGrab]);
  cset("color_slider_grab_active"_key,     style.Colors[ImGuiCol_SliderGrabActive]);
  cset("color_button"_key,                 style.Colors[ImGuiCol_Button]);
  cset("color_button_hovered"_key,         style.Colors[ImGuiCol_ButtonHovered]);
  cset("color_button_active"_key,          style.Colors[ImGuiCol_ButtonActive]);
  cset("color_header"_key,                 style.Colors[ImGuiCol_Header]);
  cset("color_header_hovered"_key,         style.Colors[ImGuiCol_HeaderHovered]);
  cset("color_header_active"_key,          style.Colors[ImGuiCol_HeaderActive]);
  cset("color_separator"_key,              style.Colors[ImGuiCol_Separator]);
  cset("color_separator_hovered"_key,      style.Colors[ImGuiCol_SeparatorHovered]);
  cset("color_separator_active"_key,       style.Colors[ImGuiCol_SeparatorActive]);
  cset("color_resize_grip"_key,            style.Colors[ImGuiCol_ResizeGrip]);
  cset("color_resize_grip_hovered"_key,    style.Colors[ImGuiCol_ResizeGripHovered]);
  cset("color_resize_grip_active"_key,     style.Colors[ImGuiCol_ResizeGripActive]);
  cset("color_plot_lines"_key,             style.Colors[ImGuiCol_PlotLines]);
  cset("color_plot_lines_hovered"_key,     style.Colors[ImGuiCol_PlotLinesHovered]);
  cset("color_plot_histogram"_key,         style.Colors[ImGuiCol_PlotHistogram]);
  cset("color_plot_histogram_hovered"_key, style.Colors[ImGuiCol_PlotHistogramHovered]);
  cset("color_text_selected_bg"_key,       style.Colors[ImGuiCol_TextSelectedBg]);
  cset("color_modal_window_dim_bg"_key,    style.Colors[ImGuiCol_ModalWindowDimBg]);
}

// ── imgui_renderer ────────────────────────────────────────────────────────────

void imgui_renderer::begin_frame() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
    io.DisplaySize = ImVec2(800.0f, 600.0f);
  if (io.DeltaTime <= 0.0f)
    io.DeltaTime = 1.0f / 60.0f;
  ImGui::NewFrame();
}

void imgui_renderer::end_frame() {
  ImGui::EndFrame();
}

void imgui_renderer::render_node(const ui_element& node, session& s) {
  if (!bool_field(node, "visible"_key, true)) return;

  auto cls = node.as<key_t>(dynamic::CLASS);

  if      (cls == "Window"_key)      render_window(*this, node, s);
  else if (cls == "Label"_key)       render_label(node);
  else if (cls == "Button"_key)      render_button(node, s);
  else if (cls == "Checkbox"_key)    render_checkbox(node, s);
  else if (cls == "SliderFloat"_key) render_slider_float(node, s);
  else if (cls == "SliderInt"_key)   render_slider_int(node, s);
  else if (cls == "InputText"_key)   render_input_text(node, s);
  else if (cls == "Image"_key)            render_image(*this, node, s);
  else if (cls == "Separator"_key)        ImGui::Separator();
  else if (cls == "VerticalLayout"_key)   render_vertical_layout(*this, node, s);
  else if (cls == "HorizontalLayout"_key) render_horizontal_layout(*this, node, s);
  else {
    // Unknown class: log and pass through so children still render.
    ImGui::TextDisabled("[wish: unknown element]");
    render_children(*this, node, s);
  }
}

void imgui_renderer::render_session(const ui_element& root, session& s) {
  if (!s.style_service) {
    render_node(root, s);
    return;
  }
  // RAII guard: save the global ImGuiStyle, apply the session's style, render,
  // then restore — so each session gets an independent theme without persisting
  // into the next session's render pass (or into the next frame's default state).
  ImGuiStyle saved = ImGui::GetStyle();
  apply_style_fields(s.style_service->current_style(), ImGui::GetStyle());
  try {
    render_node(root, s);
  } catch (...) {
    ImGui::GetStyle() = saved;
    throw;
  }
  ImGui::GetStyle() = saved;
}

ImTextureID imgui_renderer::get_or_load_texture(
    const std::string& src,
    const std::filesystem::path& resource_dir) {
  auto it = texture_cache_.find(src);
  if (it != texture_cache_.end()) return it->second;
  // Texture loading requires a GPU backend; return null in headless contexts.
  (void)resource_dir;
  return texture_cache_[src] = ImTextureID{};
}

}  // namespace bdg::wish
