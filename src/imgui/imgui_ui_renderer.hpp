// MIT License © 2025 Binary Dice Games
/// @file imgui_ui_renderer.hpp
/// @brief Declares ImGui render functions for wish UI elements.
///
/// Functions are defined in imgui_ui_renderer.cpp and have the same uniform
/// signature used throughout the imgui_renderer dispatch table:
///   void(imgui_renderer&, const ui_element&, const session&)
#pragma once

#ifdef WISH_IMGUI_ENABLED

#include <wish/file_service.hpp>
#include <wish/imgui_renderer.hpp>
#include <wish/ui_element.hpp>
#include <wish/session.hpp>

namespace bdg::wish {

// Core
void render_window(imgui_renderer& r, const ui_element& node, const session& s);
void render_label(imgui_renderer&, const ui_element& node, const session&);
void render_button(imgui_renderer&, const ui_element& node, const session& s);
void render_checkbox(imgui_renderer&, const ui_element& node, const session& s);
void render_slider_float(imgui_renderer&, const ui_element& node, const session& s);
void render_slider_int(imgui_renderer&, const ui_element& node, const session& s);
void render_input_text(imgui_renderer&, const ui_element& node, const session& s);
void render_image(imgui_renderer& r, const ui_element& node, const session& s);
void render_separator(imgui_renderer&, const ui_element&, const session&);
void render_separator_text(imgui_renderer&, const ui_element& node, const session&);
void render_vertical_layout(imgui_renderer& r, const ui_element& node, const session& s);
void render_horizontal_layout(imgui_renderer& r, const ui_element& node, const session& s);

// Menu
void render_menu_bar(imgui_renderer& r, const ui_element& node, const session& s);
void render_menu(imgui_renderer& r, const ui_element& node, const session& s);
void render_menu_item(imgui_renderer&, const ui_element& node, const session& s);

// Tabs
void render_tab_bar(imgui_renderer& r, const ui_element& node, const session& s);
void render_tab_item(imgui_renderer& r, const ui_element& node, const session& s);

// Tree
void render_tree_node(imgui_renderer& r, const ui_element& node, const session& s);
void render_collapsing_header(imgui_renderer& r, const ui_element& node, const session& s);

// Selection
void render_combo(imgui_renderer&, const ui_element& node, const session& s);
void render_radio_button(imgui_renderer&, const ui_element& node, const session& s);
void render_selectable(imgui_renderer&, const ui_element& node, const session& s);

// Numeric inputs
void render_input_int(imgui_renderer&, const ui_element& node, const session& s);
void render_input_float(imgui_renderer&, const ui_element& node, const session& s);
void render_drag_float(imgui_renderer&, const ui_element& node, const session& s);
void render_drag_int(imgui_renderer&, const ui_element& node, const session& s);

// Status
void render_progress_bar(imgui_renderer&, const ui_element& node, const session&);

// Text editor
void render_text_editor(imgui_renderer&, const ui_element& node, const session& s);

// Docking
void render_dockspace_viewport(imgui_renderer& r, const ui_element& node, const session& s);
void render_dockspace(imgui_renderer&, const ui_element& node, const session&);

// Table
void render_table(imgui_renderer& r, const ui_element& node, const session& s);
void render_table_column(imgui_renderer&, const ui_element&, const session&);
void render_table_row(imgui_renderer& r, const ui_element& node, const session& s);

}  // namespace bdg::wish

#endif  // WISH_IMGUI_ENABLED
