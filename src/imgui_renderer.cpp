// MIT License © 2025 Binary Dice Games
/// @file imgui_renderer.cpp
/// @brief Dear ImGui concrete renderer — element dispatch.
#include <wish/imgui_renderer.hpp>
#include <wish/renderer.hpp>
#include <wish/session.hpp>
#include <wish/style_service.hpp>

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_common.hpp"

#include <imgui.h>

#include "imgui_plot_renderer.hpp"
#include "imgui_plot3d_renderer.hpp"

#include <functional>
#include <string>
#include <unordered_map>
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
//
// All render functions share the same signature so they can be stored in the
// dispatch table:  void(imgui_renderer&, const ui_element&, session&)
// Container renderers use the first argument to recurse; leaf renderers ignore it.

static void render_window(
    imgui_renderer& r, const ui_element& node, session& s) {
  auto title  = str_field(node, "title"_key, "");
  int32_t px  = int_field(node, "pos_x"_key, -1);
  int32_t py  = int_field(node, "pos_y"_key, -1);
  int32_t w   = int_field(node, "width"_key, 0);
  int32_t h   = int_field(node, "height"_key, 0);
  int32_t fl  = int_field(node, "flags"_key, 0);

  // Automatically reserve menu bar space when a direct MenuBar child exists.
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    if (child.as<key_t>(dynamic::CLASS) == "MenuBar"_key)
      fl |= ImGuiWindowFlags_MenuBar;
  });

  if (px >= 0 && py >= 0)
    ImGui::SetNextWindowPos(ImVec2(float(px), float(py)), ImGuiCond_Once);
  if (w > 0 && h > 0)
    ImGui::SetNextWindowSize(ImVec2(float(w), float(h)), ImGuiCond_Once);

  if (ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags(fl)))
    render_children(r, node, s);
  ImGui::End();
}

static void render_label(imgui_renderer&, const ui_element& node, session&) {
  auto text = str_field(node, "text"_key, "");
  ImGui::TextUnformatted(text.c_str());
}

static void render_button(imgui_renderer&, const ui_element& node, session& s) {
  auto    label = str_field(node, "label"_key, "");
  int32_t w     = int_field(node, "width"_key,  0);
  int32_t h     = int_field(node, "height"_key, 0);
  if (ImGui::Button(label.c_str(), ImVec2(float(w), float(h))) && s.emit_event) {
    dynamic payload;
    s.emit_event(node_id(node), "clicked"_key, std::move(payload));
  }
}

static void render_checkbox(imgui_renderer&, const ui_element& node, session& s) {
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

static void render_slider_float(imgui_renderer&, const ui_element& node, session& s) {
  auto  label = str_field(node, "label"_key, "");
  float val   = float_field(node, "value"_key, 0.0f);
  float vmin  = float_field(node, "min"_key, 0.0f);
  float vmax  = float_field(node, "max"_key, 1.0f);
  auto  fmt   = str_field(node, "format"_key, "%.2f");
  if (ImGui::SliderFloat(label.c_str(), &val, vmin, vmax, fmt.c_str())) {
    const_cast<ui_element&>(node)["value"_key] = val;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = val;
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

static void render_slider_int(imgui_renderer&, const ui_element& node, session& s) {
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

static void render_input_text(imgui_renderer&, const ui_element& node, session& s) {
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

static void render_separator(imgui_renderer&, const ui_element&, session&) {
  ImGui::Separator();
}

static void render_separator_text(imgui_renderer&, const ui_element& node, session&) {
  auto label = str_field(node, "label"_key, "");
  ImGui::SeparatorText(label.c_str());
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

// ── Menu renderers ────────────────────────────────────────────────────────────

static void render_menu_bar(imgui_renderer& r, const ui_element& node, session& s) {
  if (ImGui::BeginMenuBar()) {
    render_children(r, node, s);
    ImGui::EndMenuBar();
  }
}

static void render_menu(imgui_renderer& r, const ui_element& node, session& s) {
  auto label   = str_field(node, "label"_key, "");
  bool enabled = bool_field(node, "enabled"_key, true);
  if (ImGui::BeginMenu(label.c_str(), enabled)) {
    render_children(r, node, s);
    ImGui::EndMenu();
  }
}

static void render_menu_item(imgui_renderer&, const ui_element& node, session& s) {
  auto  label    = str_field(node, "label"_key, "");
  auto  shortcut = str_field(node, "shortcut"_key, "");
  bool  checked  = bool_field(node, "checked"_key, false);
  bool  enabled  = bool_field(node, "enabled"_key, true);
  const char* sc = shortcut.empty() ? nullptr : shortcut.c_str();
  if (ImGui::MenuItem(label.c_str(), sc, &checked, enabled)) {
    const_cast<ui_element&>(node)["checked"_key] = checked;
    if (s.emit_event) {
      dynamic payload;
      payload["checked"_key] = checked;
      s.emit_event(node_id(node), "clicked"_key, std::move(payload));
    }
  }
}

// ── Tab renderers ─────────────────────────────────────────────────────────────

static void render_tab_bar(imgui_renderer& r, const ui_element& node, session& s) {
  auto id = str_field(node, "id"_key, "##tabbar");
  if (ImGui::BeginTabBar(id.c_str())) {
    render_children(r, node, s);
    ImGui::EndTabBar();
  }
}

static void render_tab_item(imgui_renderer& r, const ui_element& node, session& s) {
  auto  label    = str_field(node, "label"_key, "Tab");
  bool  closable = bool_field(node, "closable"_key, false);
  bool  open     = true;
  bool* p_open   = closable ? &open : nullptr;

  bool is_selected = ImGui::BeginTabItem(label.c_str(), p_open);

  // Emit 'selected' only on the transition from invisible to visible.
  const auto* prev_f = node.findField("__selected__"_key);
  bool was_selected  = (prev_f && prev_f->is<bool>()) ? prev_f->as<bool>() : is_selected;
  const_cast<ui_element&>(node)["__selected__"_key] = is_selected;
  if (is_selected && !was_selected && s.emit_event)
    s.emit_event(node_id(node), "selected"_key, dynamic{});

  if (is_selected) {
    render_children(r, node, s);
    ImGui::EndTabItem();
  }

  if (closable && !open && s.emit_event)
    s.emit_event(node_id(node), "closed"_key, dynamic{});
}

// ── Tree renderers ────────────────────────────────────────────────────────────

static void render_tree_node(imgui_renderer& r, const ui_element& node, session& s) {
  auto label     = str_field(node, "label"_key, "");
  bool init_open = bool_field(node, "open"_key, false);
  bool leaf      = bool_field(node, "leaf"_key, false);

  ImGui::SetNextItemOpen(init_open, ImGuiCond_Once);

  ImGuiTreeNodeFlags flags = leaf
      ? (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen)
      : ImGuiTreeNodeFlags_None;
  bool is_open = ImGui::TreeNodeEx(label.c_str(), flags);

  const auto* prev_f = node.findField("__open__"_key);
  bool was_open = (prev_f && prev_f->is<bool>()) ? prev_f->as<bool>() : is_open;
  const_cast<ui_element&>(node)["__open__"_key] = is_open;
  if (is_open != was_open && s.emit_event) {
    dynamic payload;
    payload["open"_key] = is_open;
    s.emit_event(node_id(node), "toggled"_key, std::move(payload));
  }

  if (is_open && !leaf) {
    render_children(r, node, s);
    ImGui::TreePop();
  }
}

static void render_collapsing_header(
    imgui_renderer& r, const ui_element& node, session& s) {
  auto label   = str_field(node, "label"_key, "");
  bool is_open = ImGui::CollapsingHeader(label.c_str());

  const auto* prev_f = node.findField("__open__"_key);
  bool was_open = (prev_f && prev_f->is<bool>()) ? prev_f->as<bool>() : is_open;
  const_cast<ui_element&>(node)["__open__"_key] = is_open;
  if (is_open != was_open && s.emit_event) {
    dynamic payload;
    payload["open"_key] = is_open;
    s.emit_event(node_id(node), "toggled"_key, std::move(payload));
  }

  if (is_open)
    render_children(r, node, s);
}

// ── Selection renderers ───────────────────────────────────────────────────────

static void render_combo(imgui_renderer&, const ui_element& node, session& s) {
  auto    label     = str_field(node, "label"_key, "");
  auto    items_str = str_field(node, "items"_key, "");
  int32_t sel       = int_field(node, "value"_key, 0);

  // Build per-frame vectors from the newline-separated items string.
  std::vector<std::string> items;
  std::string::size_type pos = 0, end;
  while ((end = items_str.find('\n', pos)) != std::string::npos) {
    items.push_back(items_str.substr(pos, end - pos));
    pos = end + 1;
  }
  if (!items_str.empty()) items.push_back(items_str.substr(pos));

  std::vector<const char*> ptrs;
  ptrs.reserve(items.size());
  for (const auto& item : items) ptrs.push_back(item.c_str());

  int cur = sel;
  if (ImGui::Combo(label.c_str(), &cur, ptrs.data(), int(ptrs.size()))) {
    const_cast<ui_element&>(node)["value"_key] = int32_t(cur);
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = int32_t(cur);
      if (cur >= 0 && cur < int(items.size()))
        payload["text"_key] = items[size_t(cur)];
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

static void render_radio_button(imgui_renderer&, const ui_element& node, session& s) {
  auto label  = str_field(node, "label"_key, "");
  bool active = bool_field(node, "active"_key, false);
  if (ImGui::RadioButton(label.c_str(), active) && s.emit_event)
    s.emit_event(node_id(node), "clicked"_key, dynamic{});
}

static void render_selectable(imgui_renderer&, const ui_element& node, session& s) {
  auto  label    = str_field(node, "label"_key, "");
  bool  selected = bool_field(node, "selected"_key, false);
  float w        = float_field(node, "width"_key, 0.0f);
  float h        = float_field(node, "height"_key, 0.0f);
  bool  v        = selected;
  if (ImGui::Selectable(label.c_str(), &v, 0, ImVec2(w, h))) {
    const_cast<ui_element&>(node)["selected"_key] = v;
    if (s.emit_event) {
      dynamic payload;
      payload["selected"_key] = v;
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

// ── Numeric input renderers ───────────────────────────────────────────────────

static void render_progress_bar(imgui_renderer&, const ui_element& node, session&) {
  float val     = float_field(node, "value"_key, 0.0f);
  float w       = float_field(node, "width"_key, -1.0f);
  float h       = float_field(node, "height"_key, 0.0f);
  auto  overlay = str_field(node, "label"_key, "");
  ImGui::ProgressBar(val, ImVec2(w, h), overlay.empty() ? nullptr : overlay.c_str());
}

static void render_input_int(imgui_renderer&, const ui_element& node, session& s) {
  auto    label     = str_field(node, "label"_key, "");
  int32_t val       = int_field(node, "value"_key, 0);
  int32_t step      = int_field(node, "step"_key, 1);
  int32_t step_fast = int_field(node, "step_fast"_key, 100);
  int v = val;
  if (ImGui::InputInt(label.c_str(), &v, step, step_fast)) {
    const_cast<ui_element&>(node)["value"_key] = int32_t(v);
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = int32_t(v);
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

static void render_input_float(imgui_renderer&, const ui_element& node, session& s) {
  auto  label     = str_field(node, "label"_key, "");
  float val       = float_field(node, "value"_key, 0.0f);
  float step      = float_field(node, "step"_key, 0.0f);
  float step_fast = float_field(node, "step_fast"_key, 0.0f);
  auto  fmt       = str_field(node, "format"_key, "%.3f");
  float v = val;
  if (ImGui::InputFloat(label.c_str(), &v, step, step_fast, fmt.c_str())) {
    const_cast<ui_element&>(node)["value"_key] = v;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = v;
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

static void render_drag_float(imgui_renderer&, const ui_element& node, session& s) {
  auto  label = str_field(node, "label"_key, "");
  float val   = float_field(node, "value"_key, 0.0f);
  float speed = float_field(node, "speed"_key, 1.0f);
  float vmin  = float_field(node, "min"_key, 0.0f);
  float vmax  = float_field(node, "max"_key, 0.0f);
  auto  fmt   = str_field(node, "format"_key, "%.3f");
  float v = val;
  if (ImGui::DragFloat(label.c_str(), &v, speed, vmin, vmax, fmt.c_str())) {
    const_cast<ui_element&>(node)["value"_key] = v;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = v;
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

static void render_drag_int(imgui_renderer&, const ui_element& node, session& s) {
  auto    label = str_field(node, "label"_key, "");
  int32_t val   = int_field(node, "value"_key, 0);
  float   speed = float_field(node, "speed"_key, 1.0f);
  int32_t vmin  = int_field(node, "min"_key, 0);
  int32_t vmax  = int_field(node, "max"_key, 0);
  int v = val;
  if (ImGui::DragInt(label.c_str(), &v, speed, vmin, vmax)) {
    const_cast<ui_element&>(node)["value"_key] = int32_t(v);
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = int32_t(v);
      s.emit_event(node_id(node), "changed"_key, std::move(payload));
    }
  }
}

// ── Dispatch table ────────────────────────────────────────────────────────────
//
// Maps class key hash → render function.  Built once at first render_node call.

using render_fn = void (*)(imgui_renderer&, const ui_element&, session&);

static const std::unordered_map<bison::hash_t, render_fn>& render_dispatch() {
  static const std::unordered_map<bison::hash_t, render_fn> tbl{
    // Core
    {"Window"_key.id,           render_window           },
    {"Label"_key.id,            render_label            },
    {"Button"_key.id,           render_button           },
    {"Checkbox"_key.id,         render_checkbox         },
    {"SliderFloat"_key.id,      render_slider_float     },
    {"SliderInt"_key.id,        render_slider_int       },
    {"InputText"_key.id,        render_input_text       },
    {"Image"_key.id,            render_image            },
    {"Separator"_key.id,        render_separator        },
    {"SeparatorText"_key.id,    render_separator_text   },
    {"VerticalLayout"_key.id,   render_vertical_layout  },
    {"HorizontalLayout"_key.id, render_horizontal_layout},
    // Menu
    {"MenuBar"_key.id,          render_menu_bar         },
    {"Menu"_key.id,             render_menu             },
    {"MenuItem"_key.id,         render_menu_item        },
    // Tabs
    {"TabBar"_key.id,           render_tab_bar          },
    {"TabItem"_key.id,          render_tab_item         },
    // Tree
    {"TreeNode"_key.id,         render_tree_node        },
    {"CollapsingHeader"_key.id, render_collapsing_header},
    // Selection
    {"Combo"_key.id,            render_combo            },
    {"RadioButton"_key.id,      render_radio_button     },
    {"Selectable"_key.id,       render_selectable       },
    // Numeric inputs
    {"InputInt"_key.id,         render_input_int        },
    {"InputFloat"_key.id,       render_input_float      },
    {"DragFloat"_key.id,        render_drag_float       },
    {"DragInt"_key.id,          render_drag_int         },
    // Status
    {"ProgressBar"_key.id,      render_progress_bar     },
    // Plot elements (require ImPlot context — must be inside a Plot)
    {"Plot"_key.id,             render_plot             },
    {"PlotLine"_key.id,         render_plot_line        },
    {"PlotScatter"_key.id,      render_plot_scatter     },
    {"PlotStairs"_key.id,       render_plot_stairs      },
    {"PlotStems"_key.id,        render_plot_stems       },
    {"PlotShaded"_key.id,       render_plot_shaded      },
    {"PlotDigital"_key.id,      render_plot_digital     },
    {"PlotBars"_key.id,         render_plot_bars        },
    {"PlotBarsH"_key.id,        render_plot_bars_h      },
    {"PlotHistogram"_key.id,    render_plot_histogram   },
    {"PlotHistogram2D"_key.id,  render_plot_histogram2d },
    {"PlotHeatmap"_key.id,      render_plot_heatmap     },
    {"PlotPieChart"_key.id,     render_plot_pie_chart   },
    {"PlotText"_key.id,         render_plot_text        },
    {"PlotInfLines"_key.id,     render_plot_inf_lines   },
    // 3-D plot elements (require ImPlot3D context — must be inside a Plot3D)
    {"Plot3D"_key.id,           render_plot3d           },
    {"Plot3DLine"_key.id,       render_plot3d_line      },
    {"Plot3DScatter"_key.id,    render_plot3d_scatter   },
    {"Plot3DSurface"_key.id,    render_plot3d_surface   },
    {"Plot3DTriangle"_key.id,   render_plot3d_triangle  },
    {"Plot3DQuad"_key.id,       render_plot3d_quad      },
    {"Plot3DMesh"_key.id,       render_plot3d_mesh      },
    {"Plot3DText"_key.id,       render_plot3d_text      },
  };
  return tbl;
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
  const auto& tbl = render_dispatch();
  auto it = tbl.find(cls.id);
  if (it != tbl.end()) {
    it->second(*this, node, s);
  } else {
    // Unknown class: log placeholder and pass through so children still render.
    ImGui::TextDisabled("[wish: unknown element '%s']",
        std::to_string(cls.id).c_str());
    render_children(*this, node, s);
  }
}

void imgui_renderer::render_session(const ui_element& root, session& s) {
  if (!s.style_service) {
    render_node(root, s);
    return;
  }

  // Recompile the bison field map into an ImGuiStyle only when the client
  // has changed the style since the last compiled cache.
  if (s.style_service->is_dirty()) {
    auto compiled = std::make_shared<ImGuiStyle>();
    apply_style_fields(s.style_service->current_style(), *compiled);
    s.style_service->set_renderer_cache(compiled);
  }

  // RAII guard: swap in the cached style, render, restore.
  const ImGuiStyle& compiled =
      *std::static_pointer_cast<ImGuiStyle>(s.style_service->renderer_cache());
  ImGuiStyle saved = ImGui::GetStyle();
  ImGui::GetStyle() = compiled;
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
