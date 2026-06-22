// MIT License © 2025 Binary Dice Games
/// @file imgui_renderer.cpp
/// @brief Dear ImGui concrete renderer — leaf element dispatch.
#include <wish/imgui_renderer.hpp>
#include <wish/renderer.hpp>
#include <wish/session.hpp>

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_common.hpp"

#include <imgui.h>

#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

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
  auto label = str_field(node, "label"_key, "");
  if (ImGui::Button(label.c_str()) && s.emit_event) {
    dynamic payload;
    s.emit_event(
        node.as<key_t>(dynamic::CLASS), "clicked"_key, std::move(payload));
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
      s.emit_event(
          node.as<key_t>(dynamic::CLASS), "changed"_key, std::move(payload));
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
      s.emit_event(
          node.as<key_t>(dynamic::CLASS), "changed"_key, std::move(payload));
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
      s.emit_event(
          node.as<key_t>(dynamic::CLASS), "changed"_key, std::move(payload));
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
      s.emit_event(
          node.as<key_t>(dynamic::CLASS), "changed"_key, std::move(payload));
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
