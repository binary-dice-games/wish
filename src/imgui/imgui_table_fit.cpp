// MIT License © 2026 Binary Dice Games
/// @file imgui_table_fit.cpp
/// @brief Implementation of table_fit_fill_columns().
#include "imgui_table_fit.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <unordered_map>

namespace bdg::wish {

namespace {

// Tables whose columns the user has resized: their fill columns are left
// alone from then on. Keyed like imgui_ui_renderer.cpp's per-table caches.
std::unordered_map<uint32_t, bool>& user_sized_tables() {
  static std::unordered_map<uint32_t, bool> cache;
  return cache;
}

} // namespace

void table_fit_fill_columns(uint32_t table_key, const std::vector<table_fill_column>& fill) {
  ImGuiTable* table = ImGui::GetCurrentTable();
  if (!table || fill.empty())
    return;

  bool& user_sized = user_sized_tables()[table_key];
  // LastResizedColumn: the column whose border was dragged (or double-clicked)
  // on the previous frame; that width has already been applied by BeginTable().
  if (table->LastResizedColumn != -1 || table->ResizedColumn != -1)
    user_sized = true;
  if (user_sized)
    return;

  auto is_fill = [&](int n) {
    return std::any_of(fill.begin(), fill.end(), [n](const table_fill_column& f) { return f.index == n; });
  };

  // What the other columns take, plus every enabled column's padding/spacing.
  float taken = table->OuterPaddingX * 2.0f;
  float total_weight = 0.0f;
  for (int n = 0; n < table->ColumnsCount; ++n) {
    const ImGuiTableColumn& column = table->Columns[n];
    if (!column.IsUserEnabled)
      continue;
    taken += table->CellPaddingX * 2.0f + table->CellSpacingX1 + table->CellSpacingX2;
    if (is_fill(n))
      continue;
    if (column.WidthRequest < 0.0f)
      return; // not sized yet (its first frame): try again next frame.
    taken += column.WidthRequest;
  }
  for (auto& f : fill) {
    if (f.index >= 0 && f.index < table->ColumnsCount && table->Columns[f.index].IsUserEnabled)
      total_weight += f.weight;
  }
  if (total_weight <= 0.0f)
    return;

  // The visible width: the outer rect less the vertical scrollbar, when shown.
  float visible = table->OuterRect.GetWidth();
  if (table->InnerWindow && table->InnerWindow != table->OuterWindow)
    visible -= table->InnerWindow->ScrollbarSizes.x;

  const float spare = visible - taken;
  const float min_width = std::max(table->MinColumnWidth, 60.0f);
  for (auto& f : fill) {
    if (f.index < 0 || f.index >= table->ColumnsCount)
      continue;
    ImGuiTableColumn& column = table->Columns[f.index];
    if (!column.IsUserEnabled)
      continue;
    // Floor: a fractional width would leave a 1px horizontal scroll range.
    column.WidthRequest = std::max(min_width, ImFloor(spare * f.weight / total_weight));
    column.AutoFitQueue = 0x00; // our width, not the content's.
  }
}

} // namespace bdg::wish
