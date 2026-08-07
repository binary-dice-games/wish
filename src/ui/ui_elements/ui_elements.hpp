// MIT License © 2025 Binary Dice Games
/// @file ui_elements.hpp
/// @brief Internal declarations for per-element registration functions.
///        Not part of the public wish API.
#pragma once

#include <ui/ui_element.hpp>

namespace bdg::wish {

void register_element();
void register_layout();
void register_window();
void register_label();
void register_button();
void register_checkbox();
void register_slider();
void register_input_text();
void register_image();
void register_separator(); // Separator + SeparatorText
void register_menu(); // MenuBar, Menu, MenuItem, MenuButton
void register_tabs(); // TabBar, TabItem
void register_tree(); // TreeNode, CollapsingHeader
void register_combo();
void register_radio_button();
void register_progress_bar();
void register_input_number(); // InputInt, InputFloat
void register_drag(); // DragFloat, DragInt
void register_selectable();
void register_docking(); // DockSpaceViewport, DockSpace
void register_table(); // Table, TableColumn, TableRow
void register_text_editor();
void register_graph_node();

} // namespace bdg::wish
