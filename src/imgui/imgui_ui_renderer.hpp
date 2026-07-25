// MIT License © 2025 Binary Dice Games
/// @file imgui_ui_renderer.hpp
/// @brief Declares ImGui render functions for wish UI elements.
///
/// Functions are defined in imgui_ui_renderer.cpp and have the same uniform
/// signature used throughout the imgui_renderer dispatch table:
///   void(imgui_renderer&, const ui_element&, const context&)
#pragma once

#ifdef WISH_IMGUI_ENABLED

#include <context/file_service.hpp>
#include <imgui/imgui_renderer.hpp>
#include <context/context.hpp>
#include <ui/ui_element.hpp>

#include <cstdint>

namespace bdg::wish {

// ── ID helper ─────────────────────────────────────────────────────────────────

/// @brief Stable per-element identity for use as an ImGui widget ID.
///
/// Returns the element's "__path__" field verbatim (e.g. "dialog.ok", or a
/// form's ordinally-assigned root key like "__notepad_0" -- see
/// form::next_available_key() in ui/forms/form.hpp) when present: it is
/// stamped at template-registration/form-init time and constant across
/// process runs, so the same window/widget gets the same ImGui ID every
/// run and ImGui's saved layout (imgui.ini) actually matches up across
/// restarts. Returned as a string rather than a hash of it so that a
/// Window's saved "[Window][...]" section in imgui.ini -- which ImGui
/// truncates to just the "###" suffix, see with_id() in
/// imgui_ui_renderer.cpp -- stays human-readable.
///
/// Falls back to the element's "__wish_id" RMI id (assigned fresh every
/// run) when no "__path__" is present -- e.g. dynamically-created children,
/// which do not have a stable definition-time path.
std::string stable_id(const ui_element& node);

// Core
void render_window(imgui_renderer& r, const ui_element& node, const context& s);
void render_label(imgui_renderer&, const ui_element& node, const context&);
void render_button(imgui_renderer&, const ui_element& node, const context& s);
void render_checkbox(imgui_renderer&, const ui_element& node, const context& s);
void render_slider_float(imgui_renderer&, const ui_element& node, const context& s);
void render_slider_int(imgui_renderer&, const ui_element& node, const context& s);
void render_input_text(imgui_renderer&, const ui_element& node, const context& s);
void render_image(imgui_renderer& r, const ui_element& node, const context& s);
void render_separator(imgui_renderer&, const ui_element&, const context&);
void render_separator_text(imgui_renderer&, const ui_element& node, const context&);
void render_vertical_layout(imgui_renderer& r, const ui_element& node, const context& s);
void render_horizontal_layout(imgui_renderer& r, const ui_element& node, const context& s);

// Menu
void render_menu_bar(imgui_renderer& r, const ui_element& node, const context& s);
void render_menu(imgui_renderer& r, const ui_element& node, const context& s);
void render_menu_item(imgui_renderer&, const ui_element& node, const context& s);
void render_menu_button(imgui_renderer& r, const ui_element& node, const context& s);

// Tabs
void render_tab_bar(imgui_renderer& r, const ui_element& node, const context& s);
void render_tab_item(imgui_renderer& r, const ui_element& node, const context& s);

// Tree
void render_tree_node(imgui_renderer& r, const ui_element& node, const context& s);
void render_collapsing_header(imgui_renderer& r, const ui_element& node, const context& s);

// Selection
void render_combo(imgui_renderer&, const ui_element& node, const context& s);
void render_radio_button(imgui_renderer&, const ui_element& node, const context& s);
void render_selectable(imgui_renderer&, const ui_element& node, const context& s);

// Numeric inputs
void render_input_int(imgui_renderer&, const ui_element& node, const context& s);
void render_input_float(imgui_renderer&, const ui_element& node, const context& s);
void render_drag_float(imgui_renderer&, const ui_element& node, const context& s);
void render_drag_int(imgui_renderer&, const ui_element& node, const context& s);

// Status
void render_progress_bar(imgui_renderer&, const ui_element& node, const context&);

// Text editor
void render_text_editor(imgui_renderer&, const ui_element& node, const context& s);

// Docking
void render_dockspace_viewport(imgui_renderer& r, const ui_element& node, const context& s);
void render_dockspace(imgui_renderer&, const ui_element& node, const context&);

// Table
void render_table(imgui_renderer& r, const ui_element& node, const context& s);
void render_table_column(imgui_renderer&, const ui_element&, const context&);
void render_table_row(imgui_renderer& r, const ui_element& node, const context& s);

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
