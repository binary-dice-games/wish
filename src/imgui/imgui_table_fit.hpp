// MIT License © 2026 Binary Dice Games
/// @file imgui_table_fit.hpp
/// @brief Fill-column sizing for a Table whose `resize_pushes` field is set.
#pragma once

#include <cstdint>
#include <vector>

namespace bdg::wish {

/// @brief One column that should take up the table's spare width.
struct table_fill_column {
  int index;    ///< Column index, in TableSetupColumn() order.
  float weight; ///< Share of the spare width relative to the other fill columns (> 0).
};

/// @brief Sizes the current table's fill columns so the columns exactly fill
/// the table's visible width -- until the user resizes any column.
///
/// A `resize_pushes` Table declares every column `WidthFixed` to ImGui (with
/// `ScrollX`, that is what makes dragging a column border push the following
/// columns instead of borrowing width from a neighbour). Its `WidthStretch`
/// columns are passed here instead: each frame they are given whatever width
/// the fixed ones leave over, so the table still tracks its window like a
/// stretch layout would. The first user resize of any column ends that for
/// the rest of the session -- from then on every column keeps its width and
/// the table scrolls horizontally.
///
/// Call between the table's last `ImGui::TableSetupColumn()` and its first
/// row / header. Lives in its own translation unit because it reads
/// `ImGuiTable` (imgui_internal.h), which imgui_ui_renderer.cpp does not
/// include (see imgui_dock_layout.cpp for the same split).
///
/// @param table_key  Stable per-table key (the Table's wish id).
/// @param fill       The fill columns; empty is a no-op.
void table_fit_fill_columns(uint32_t table_key, const std::vector<table_fill_column>& fill);

} // namespace bdg::wish
