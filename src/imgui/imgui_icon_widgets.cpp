// MIT License © 2026 Binary Dice Games
/// @file imgui_icon_widgets.cpp
/// @brief Implementation of icon_button() / icon_menu_item().
///
/// Lives in its own translation unit because icon_menu_item() reads the
/// popup's menu-column layout (`ImGuiWindow::DC.MenuColumns`) and calls
/// `ImGui::MenuItemEx()`, both from imgui_internal.h, which
/// imgui_ui_renderer.cpp does not include (see imgui_dock_layout.cpp /
/// imgui_table_fit.cpp for the same split).
#include "imgui_icon_widgets.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace bdg::wish {

bool icon_button(const char* label, ImTextureID icon, const ImVec4& tint, const ImVec2& size) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float icon_px = ImGui::GetTextLineHeight();
  const char* label_end = ImGui::FindRenderedTextEnd(label);
  const ImVec2 text_size = ImGui::CalcTextSize(label, label_end, false);
  const bool has_text = label != label_end;
  const float gap = has_text ? style.ItemInnerSpacing.x : 0.0f;
  const float content_w = icon_px + gap + text_size.x;

  // An icon-only button is square; otherwise ImGui::Button()'s auto size.
  const float frame_h = ImGui::GetFrameHeight();
  const float auto_w = has_text ? content_w + style.FramePadding.x * 2.0f : frame_h;
  const ImVec2 button_size = ImGui::CalcItemSize(size, auto_w, frame_h);

  // The frame (and its hover/active states and the ImGui id) is a plain
  // label-less Button; icon and text are drawn over it. The id is scoped by
  // the per-node PushID in imgui_renderer::render_node().
  const bool pressed = ImGui::Button("##icon_button", button_size);

  const ImVec2 bb_min = ImGui::GetItemRectMin();
  const ImVec2 bb_max = ImGui::GetItemRectMax();
  const float inner_w = bb_max.x - bb_min.x - style.FramePadding.x * 2.0f;
  const float x = bb_min.x + style.FramePadding.x + std::max(0.0f, inner_w - content_w) * style.ButtonTextAlign.x;
  const float y = bb_min.y + std::floor((bb_max.y - bb_min.y - icon_px) * 0.5f);
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  draw_list->PushClipRect(bb_min, bb_max, true);
  draw_list->AddImage(
      icon, ImVec2(x, y), ImVec2(x + icon_px, y + icon_px), ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(tint));
  if (has_text) {
    const ImVec2 text_pos(x + icon_px + gap, bb_min.y + std::floor((bb_max.y - bb_min.y - text_size.y) * 0.5f));
    draw_list->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text), label, label_end);
  }
  draw_list->PopClipRect();
  return pressed;
}

bool icon_menu_item(const char* label, const char* shortcut, bool selected, bool enabled, ImTextureID icon) {
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (!icon || window->DC.LayoutType == ImGuiLayoutType_Horizontal)
    return ImGui::MenuItem(label, shortcut, selected, enabled);

  // MenuItemEx() takes a *text* icon and reserves its width in the popup's
  // shared icon column -- that column is what keeps every item's label
  // aligned. Pass blanks as wide as the image and draw the image over them.
  const float icon_px = ImGui::GetTextLineHeight();
  const float space_w = ImGui::CalcTextSize(" ").x;
  static thread_local std::string blanks;
  blanks.assign(space_w > 0.0f ? static_cast<size_t>(std::ceil(icon_px / space_w)) : 1U, ' ');

  const ImVec2 pos = window->DC.CursorPos;
  const bool pressed = ImGui::MenuItemEx(label, blanks.c_str(), shortcut, selected, enabled);

  const float x = pos.x + window->DC.MenuColumns.OffsetIcon;
  const ImVec2 bb_min = ImGui::GetItemRectMin();
  const ImVec2 bb_max = ImGui::GetItemRectMax();
  const float y = bb_min.y + std::floor((bb_max.y - bb_min.y - icon_px) * 0.5f);
  const ImU32 tint = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
  window->DrawList->AddImage(icon, ImVec2(x, y), ImVec2(x + icon_px, y + icon_px), ImVec2(0, 0), ImVec2(1, 1), tint);
  return pressed;
}

} // namespace bdg::wish
