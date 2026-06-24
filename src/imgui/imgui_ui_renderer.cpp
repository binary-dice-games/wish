// MIT License © 2025 Binary Dice Games
/// @file imgui_ui_renderer.cpp
/// @brief ImGui render functions for wish UI elements.
///
/// Each function maps one wish element class to the corresponding ImGui call.
/// All functions share the signature:
///   void(imgui_renderer&, const ui_element&, session&)
/// matching the render_fn typedef in imgui_renderer.cpp.
#include "imgui_ui_renderer.hpp"

#ifdef WISH_IMGUI_ENABLED

#include <wish/renderer.hpp>

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

// ── Core ──────────────────────────────────────────────────────────────────────

void render_window(imgui_renderer& r, const ui_element& node, session& s) {
  auto title  = node.get_as<std::string>("title"_key, "");
  int32_t px  = node.get_as<int32_t>("pos_x"_key, -1);
  int32_t py  = node.get_as<int32_t>("pos_y"_key, -1);
  int32_t w   = node.get_as<int32_t>("width"_key, 0);
  int32_t h   = node.get_as<int32_t>("height"_key, 0);
  int32_t fl  = node.get_as<int32_t>("flags"_key, 0);

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

void render_label(imgui_renderer&, const ui_element& node, session&) {
  auto text = node.get_as<std::string>("text"_key, "");
  ImGui::TextUnformatted(text.c_str());
}

void render_button(imgui_renderer&, const ui_element& node, session& s) {
  auto    label = node.get_as<std::string>("label"_key, "");
  int32_t w     = node.get_as<int32_t>("width"_key,  0);
  int32_t h     = node.get_as<int32_t>("height"_key, 0);
  if (ImGui::Button(label.c_str(), ImVec2(float(w), float(h))) && s.emit_event) {
    dynamic payload;
    s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "clicked"_key, std::move(payload));
  }
}

void render_checkbox(imgui_renderer&, const ui_element& node, session& s) {
  auto label = node.get_as<std::string>("label"_key, "");
  bool val   = node.get_as<bool>("value"_key, false);
  if (ImGui::Checkbox(label.c_str(), &val)) {
    const_cast<ui_element&>(node)["value"_key] = val;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = val;
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
    }
  }
}

void render_slider_float(imgui_renderer&, const ui_element& node, session& s) {
  auto  label = node.get_as<std::string>("label"_key, "");
  float val   = node.get_as<float>("value"_key, 0.0f);
  float vmin  = node.get_as<float>("min"_key, 0.0f);
  float vmax  = node.get_as<float>("max"_key, 1.0f);
  auto  fmt   = node.get_as<std::string>("format"_key, "%.2f");
  if (ImGui::SliderFloat(label.c_str(), &val, vmin, vmax, fmt.c_str())) {
    const_cast<ui_element&>(node)["value"_key] = val;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = val;
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
    }
  }
}

void render_slider_int(imgui_renderer&, const ui_element& node, session& s) {
  auto    label = node.get_as<std::string>("label"_key, "");
  int32_t val   = node.get_as<int32_t>("value"_key, 0);
  int32_t vmin  = node.get_as<int32_t>("min"_key, 0);
  int32_t vmax  = node.get_as<int32_t>("max"_key, 100);
  if (ImGui::SliderInt(label.c_str(), &val, vmin, vmax)) {
    const_cast<ui_element&>(node)["value"_key] = val;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = val;
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
    }
  }
}

void render_input_text(imgui_renderer&, const ui_element& node, session& s) {
  auto    label   = node.get_as<std::string>("label"_key, "");
  auto    hint    = node.get_as<std::string>("hint"_key, "");
  int32_t maxlen  = node.get_as<int32_t>("max_length"_key, 256);
  auto    current = node.get_as<std::string>("value"_key, "");

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
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
    }
  }
}

void render_image(imgui_renderer& r, const ui_element& node, session& s) {
  auto    src = node.get_as<std::string>("src"_key, "");
  int32_t w   = node.get_as<int32_t>("width"_key, 0);
  int32_t h   = node.get_as<int32_t>("height"_key, 0);
  if (src.empty() || w <= 0 || h <= 0) return;
  auto full_path = file_service::resolve_path(src, s.resource_dir,
                                              s.allow_absolute_paths);
  if (full_path.empty()) return;
  ImTextureID tex = r.get_or_load_texture(full_path.string(), s.resource_dir);
  if (!tex) return;
  ImGui::Image(tex, ImVec2(float(w), float(h)));
}

void render_separator(imgui_renderer&, const ui_element&, session&) {
  ImGui::Separator();
}

void render_separator_text(imgui_renderer&, const ui_element& node, session&) {
  auto label = node.get_as<std::string>("label"_key, "");
  ImGui::SeparatorText(label.c_str());
}

void render_vertical_layout(
    imgui_renderer& r, const ui_element& node, session& s) {
  float spacing = node.get_as<float>("spacing"_key, 0.0f);
  bool first = true;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    if (!first && spacing > 0.0f)
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + spacing);
    first = false;
    r.render_node(child, s);
  });
}

void render_horizontal_layout(
    imgui_renderer& r, const ui_element& node, session& s) {
  float spacing = node.get_as<float>("spacing"_key, 0.0f);
  ImGui::BeginGroup();
  bool first = true;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    if (!first) ImGui::SameLine(0.0f, spacing);
    first = false;
    r.render_node(child, s);
  });
  ImGui::EndGroup();
}

// ── Menu ──────────────────────────────────────────────────────────────────────

void render_menu_bar(imgui_renderer& r, const ui_element& node, session& s) {
  if (ImGui::BeginMenuBar()) {
    render_children(r, node, s);
    ImGui::EndMenuBar();
  }
}

void render_menu(imgui_renderer& r, const ui_element& node, session& s) {
  auto label   = node.get_as<std::string>("label"_key, "");
  bool enabled = node.get_as<bool>("enabled"_key, true);
  if (ImGui::BeginMenu(label.c_str(), enabled)) {
    render_children(r, node, s);
    ImGui::EndMenu();
  }
}

void render_menu_item(imgui_renderer&, const ui_element& node, session& s) {
  auto  label    = node.get_as<std::string>("label"_key, "");
  auto  shortcut = node.get_as<std::string>("shortcut"_key, "");
  bool  checked  = node.get_as<bool>("checked"_key, false);
  bool  enabled  = node.get_as<bool>("enabled"_key, true);
  const char* sc = shortcut.empty() ? nullptr : shortcut.c_str();
  if (ImGui::MenuItem(label.c_str(), sc, &checked, enabled)) {
    const_cast<ui_element&>(node)["checked"_key] = checked;
    if (s.emit_event) {
      dynamic payload;
      payload["checked"_key] = checked;
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "clicked"_key, std::move(payload));
    }
  }
}

// ── Tabs ──────────────────────────────────────────────────────────────────────

void render_tab_bar(imgui_renderer& r, const ui_element& node, session& s) {
  auto id = node.get_as<std::string>("id"_key, "##tabbar");
  if (ImGui::BeginTabBar(id.c_str())) {
    render_children(r, node, s);
    ImGui::EndTabBar();
  }
}

void render_tab_item(imgui_renderer& r, const ui_element& node, session& s) {
  auto  label    = node.get_as<std::string>("label"_key, "Tab");
  bool  closable = node.get_as<bool>("closable"_key, false);
  bool  open     = true;
  bool* p_open   = closable ? &open : nullptr;

  bool is_selected = ImGui::BeginTabItem(label.c_str(), p_open);

  // Emit 'selected' only on the transition from invisible to visible.
  const auto* prev_f = node.findField("__selected__"_key);
  bool was_selected  = (prev_f && prev_f->is<bool>()) ? prev_f->as<bool>() : is_selected;
  const_cast<ui_element&>(node)["__selected__"_key] = is_selected;
  if (is_selected && !was_selected && s.emit_event)
    s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "selected"_key, dynamic{});

  if (is_selected) {
    render_children(r, node, s);
    ImGui::EndTabItem();
  }

  if (closable && !open && s.emit_event)
    s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "closed"_key, dynamic{});
}

// ── Tree ──────────────────────────────────────────────────────────────────────

void render_tree_node(imgui_renderer& r, const ui_element& node, session& s) {
  auto label     = node.get_as<std::string>("label"_key, "");
  bool init_open = node.get_as<bool>("open"_key, false);
  bool leaf      = node.get_as<bool>("leaf"_key, false);

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
    s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "toggled"_key, std::move(payload));
  }

  if (is_open && !leaf) {
    render_children(r, node, s);
    ImGui::TreePop();
  }
}

void render_collapsing_header(
    imgui_renderer& r, const ui_element& node, session& s) {
  auto label   = node.get_as<std::string>("label"_key, "");
  bool is_open = ImGui::CollapsingHeader(label.c_str());

  const auto* prev_f = node.findField("__open__"_key);
  bool was_open = (prev_f && prev_f->is<bool>()) ? prev_f->as<bool>() : is_open;
  const_cast<ui_element&>(node)["__open__"_key] = is_open;
  if (is_open != was_open && s.emit_event) {
    dynamic payload;
    payload["open"_key] = is_open;
    s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "toggled"_key, std::move(payload));
  }

  if (is_open)
    render_children(r, node, s);
}

// ── Selection ─────────────────────────────────────────────────────────────────

void render_combo(imgui_renderer&, const ui_element& node, session& s) {
  auto    label     = node.get_as<std::string>("label"_key, "");
  auto    items_str = node.get_as<std::string>("items"_key, "");
  int32_t sel       = node.get_as<int32_t>("value"_key, 0);

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
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
    }
  }
}

void render_radio_button(imgui_renderer&, const ui_element& node, session& s) {
  auto label  = node.get_as<std::string>("label"_key, "");
  bool active = node.get_as<bool>("active"_key, false);
  if (ImGui::RadioButton(label.c_str(), active) && s.emit_event)
    s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "clicked"_key, dynamic{});
}

void render_selectable(imgui_renderer&, const ui_element& node, session& s) {
  auto  label    = node.get_as<std::string>("label"_key, "");
  bool  selected = node.get_as<bool>("selected"_key, false);
  float w        = node.get_as<float>("width"_key, 0.0f);
  float h        = node.get_as<float>("height"_key, 0.0f);
  bool  v        = selected;
  if (ImGui::Selectable(label.c_str(), &v, 0, ImVec2(w, h))) {
    const_cast<ui_element&>(node)["selected"_key] = v;
    if (s.emit_event) {
      dynamic payload;
      payload["selected"_key] = v;
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
    }
  }
}

// ── Numeric inputs ────────────────────────────────────────────────────────────

void render_input_int(imgui_renderer&, const ui_element& node, session& s) {
  auto    label     = node.get_as<std::string>("label"_key, "");
  int32_t val       = node.get_as<int32_t>("value"_key, 0);
  int32_t step      = node.get_as<int32_t>("step"_key, 1);
  int32_t step_fast = node.get_as<int32_t>("step_fast"_key, 100);
  int v = val;
  if (ImGui::InputInt(label.c_str(), &v, step, step_fast)) {
    const_cast<ui_element&>(node)["value"_key] = int32_t(v);
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = int32_t(v);
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
    }
  }
}

void render_input_float(imgui_renderer&, const ui_element& node, session& s) {
  auto  label     = node.get_as<std::string>("label"_key, "");
  float val       = node.get_as<float>("value"_key, 0.0f);
  float step      = node.get_as<float>("step"_key, 0.0f);
  float step_fast = node.get_as<float>("step_fast"_key, 0.0f);
  auto  fmt       = node.get_as<std::string>("format"_key, "%.3f");
  float v = val;
  if (ImGui::InputFloat(label.c_str(), &v, step, step_fast, fmt.c_str())) {
    const_cast<ui_element&>(node)["value"_key] = v;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = v;
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
    }
  }
}

void render_drag_float(imgui_renderer&, const ui_element& node, session& s) {
  auto  label = node.get_as<std::string>("label"_key, "");
  float val   = node.get_as<float>("value"_key, 0.0f);
  float speed = node.get_as<float>("speed"_key, 1.0f);
  float vmin  = node.get_as<float>("min"_key, 0.0f);
  float vmax  = node.get_as<float>("max"_key, 0.0f);
  auto  fmt   = node.get_as<std::string>("format"_key, "%.3f");
  float v = val;
  if (ImGui::DragFloat(label.c_str(), &v, speed, vmin, vmax, fmt.c_str())) {
    const_cast<ui_element&>(node)["value"_key] = v;
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = v;
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
    }
  }
}

void render_drag_int(imgui_renderer&, const ui_element& node, session& s) {
  auto    label = node.get_as<std::string>("label"_key, "");
  int32_t val   = node.get_as<int32_t>("value"_key, 0);
  float   speed = node.get_as<float>("speed"_key, 1.0f);
  int32_t vmin  = node.get_as<int32_t>("min"_key, 0);
  int32_t vmax  = node.get_as<int32_t>("max"_key, 0);
  int v = val;
  if (ImGui::DragInt(label.c_str(), &v, speed, vmin, vmax)) {
    const_cast<ui_element&>(node)["value"_key] = int32_t(v);
    if (s.emit_event) {
      dynamic payload;
      payload["value"_key] = int32_t(v);
      s.emit_event(node.get_as<key_t>("__wish_id"_key, key_t{}), "changed"_key, std::move(payload));
    }
  }
}

// ── Status ────────────────────────────────────────────────────────────────────

void render_progress_bar(imgui_renderer&, const ui_element& node, session&) {
  float val     = node.get_as<float>("value"_key, 0.0f);
  float w       = node.get_as<float>("width"_key, -1.0f);
  float h       = node.get_as<float>("height"_key, 0.0f);
  auto  overlay = node.get_as<std::string>("label"_key, "");
  ImGui::ProgressBar(val, ImVec2(w, h), overlay.empty() ? nullptr : overlay.c_str());
}

// ── Docking ───────────────────────────────────────────────────────────────────

void render_dockspace_viewport(
    imgui_renderer& r, const ui_element& node, session& s) {
  auto    id       = node.get_as<std::string>("id"_key, "##viewport_dockspace");
  int32_t flags    = node.get_as<int32_t>("flags"_key, 0);
  bool    passthru = node.get_as<bool>("passthru"_key, false);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

  // Reserve menu bar space if any direct child is a MenuBar.
  ImGuiWindowFlags host_flags =
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoNavFocus;
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    if (child.as<bison::key_t>(bison::dynamic::CLASS) == "MenuBar"_key)
      host_flags |= ImGuiWindowFlags_MenuBar;
  });

  ImGui::Begin(id.c_str(), nullptr, host_flags);
  ImGui::PopStyleVar(3);

  ImGuiDockNodeFlags dock_flags = ImGuiDockNodeFlags(flags);
  if (passthru) dock_flags |= ImGuiDockNodeFlags_PassthruCentralNode;
  ImGui::DockSpace(ImGui::GetID(id.c_str()), ImVec2(0.0f, 0.0f), dock_flags);

  // Non-Window children (e.g. MenuBar) are rendered inside the host window.
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    if (child.as<bison::key_t>(bison::dynamic::CLASS) != "Window"_key)
      r.render_node(child, s);
  });

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

void render_dockspace(imgui_renderer&, const ui_element& node, session&) {
  auto    id     = node.get_as<std::string>("id"_key, "dockspace");
  float   width  = node.get_as<float>("width"_key, 0.0f);
  float   height = node.get_as<float>("height"_key, 0.0f);
  int32_t flags  = node.get_as<int32_t>("flags"_key, 0);
  ImGui::DockSpace(
      ImGui::GetID(id.c_str()), ImVec2(width, height), ImGuiDockNodeFlags(flags));
}

// ── Table elements ────────────────────────────────────────────────────────────

void render_table(imgui_renderer& r, const ui_element& node, session& s) {
  auto    id       = node.get_as<std::string>("id"_key, "##table");
  int32_t columns  = node.get_as<int32_t>("columns"_key, 1);
  int32_t flags    = node.get_as<int32_t>("flags"_key, 0);
  float   outer_w  = node.get_as<float>("outer_width"_key, 0.0f);
  float   outer_h  = node.get_as<float>("outer_height"_key, 0.0f);
  float   inner_w  = node.get_as<float>("inner_width"_key, 0.0f);
  bool    headers  = node.get_as<bool>("headers"_key, false);

  if (!ImGui::BeginTable(id.c_str(), columns, ImGuiTableFlags(flags),
                         ImVec2(outer_w, outer_h), inner_w))
    return;

  // Column setup must precede any row; iterate TableColumn children first.
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    if (child.as<bison::key_t>(bison::dynamic::CLASS) != "TableColumn"_key)
      return;
    auto    label   = child.get_as<std::string>("label"_key, "");
    int32_t col_fl  = child.get_as<int32_t>("flags"_key, 0);
    float   col_w   = child.get_as<float>("init_width"_key, 0.0f);
    ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags(col_fl), col_w);
  });

  if (headers)
    ImGui::TableHeadersRow();

  // Render remaining children (rows and other content) in declaration order.
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    if (child.as<bison::key_t>(bison::dynamic::CLASS) == "TableColumn"_key)
      return;
    r.render_node(child, s);
  });

  ImGui::EndTable();
}

void render_table_column(imgui_renderer&, const ui_element&, session&) {
  // Handled inline by render_table during column setup; no-op when standalone.
}

void render_table_row(imgui_renderer& r, const ui_element& node, session& s) {
  int32_t flags      = node.get_as<int32_t>("flags"_key, 0);
  float   min_height = node.get_as<float>("min_height"_key, 0.0f);
  ImGui::TableNextRow(ImGuiTableRowFlags(flags), min_height);
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) {
    ImGui::TableNextColumn();
    r.render_node(child, s);
  });
}

}  // namespace bdg::wish

#endif  // WISH_IMGUI_ENABLED
