// MIT License © 2026 Binary Dice Games
/// @file imgui_window_caption.cpp
/// @brief Windows-style caption buttons, border and shadow (see
///        imgui_window_caption.hpp).
///
/// Uses imgui_internal.h for the title-bar rectangle, ImGui's own
/// title-highlight rule and `ItemAdd()`/`ButtonBehavior()`, so the buttons
/// are real items (hover, press, no window drag) that never enter keyboard
/// navigation and never affect the window's content layout.

#include "imgui_window_caption.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace bdg::wish {

namespace {

// Windows 10/11 caption colors for the close button.
constexpr ImU32 kCloseHovered = IM_COL32(232, 17, 35, 255);
constexpr ImU32 kCloseHeld = IM_COL32(241, 112, 122, 255);

enum class glyph { minimize, maximize, restore, close };

// Same rule ImGui uses for the title bar color, so the border matches it.
bool title_is_highlighted(const ImGuiWindow* window) {
  const ImGuiContext& g = *GImGui;
  const ImGuiWindow* target = g.NavWindowingTarget ? g.NavWindowingTarget : g.NavWindow;
  return target &&
      (window->RootWindowForTitleBarHighlight == target->RootWindowForTitleBarHighlight ||
       (window->DockNode && window->DockNode == target->DockNode));
}

void draw_glyph(ImDrawList* dl, glyph kind, ImVec2 center, float size, ImU32 col) {
  // Pixel-snapped so 1px lines stay crisp.
  const float h = std::floor(size * 0.5f);
  const float cx = std::floor(center.x) + 0.5f;
  const float cy = std::floor(center.y) + 0.5f;
  switch (kind) {
  case glyph::minimize:
    dl->AddLine(ImVec2(cx - h, cy), ImVec2(cx + h + 1.0f, cy), col);
    break;
  case glyph::maximize:
    dl->AddRect(ImVec2(cx - h, cy - h), ImVec2(cx + h, cy + h), col);
    break;
  case glyph::restore: {
    // A front square, and the top/right edges of one behind it.
    const float o = std::max(2.0f, std::floor(size * 0.2f));
    dl->AddRect(ImVec2(cx - h, cy - h + o), ImVec2(cx + h - o, cy + h), col);
    dl->AddLine(ImVec2(cx - h + o, cy - h + o), ImVec2(cx - h + o, cy - h), col);
    dl->AddLine(ImVec2(cx - h + o, cy - h), ImVec2(cx + h, cy - h), col);
    dl->AddLine(ImVec2(cx + h, cy - h), ImVec2(cx + h, cy + h - o), col);
    dl->AddLine(ImVec2(cx + h, cy + h - o), ImVec2(cx + h - o, cy + h - o), col);
    break;
  }
  case glyph::close:
    dl->AddLine(ImVec2(cx - h, cy - h), ImVec2(cx + h, cy + h), col);
    dl->AddLine(ImVec2(cx - h, cy + h), ImVec2(cx + h, cy - h), col);
    break;
  }
}

// One caption button; true when clicked. A disabled button still occupies
// its slot (as in Windows) but never reacts.
bool caption_button(ImGuiWindow* window, const char* str_id, const ImRect& bb, glyph kind, bool enabled,
    bool& hovered_out) {
  const ImGuiID id = window->GetID(str_id);
  if (!ImGui::ItemAdd(bb, id, nullptr, ImGuiItemFlags_NoNav | (enabled ? 0 : ImGuiItemFlags_Disabled)))
    return false;
  bool hovered = false;
  bool held = false;
  const bool pressed = enabled && ImGui::ButtonBehavior(bb, id, &hovered, &held);
  hovered = hovered && enabled;
  hovered_out = hovered_out || hovered;

  ImDrawList* dl = window->DrawList;
  const bool is_close = kind == glyph::close;
  ImU32 text = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
  if (hovered || held) {
    ImU32 bg;
    if (is_close) {
      bg = held ? kCloseHeld : kCloseHovered;
      text = IM_COL32_WHITE;
    } else {
      ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_Text);
      c.w = held ? 0.2f : 0.1f;
      bg = ImGui::GetColorU32(c);
    }
    dl->AddRectFilled(bb.Min, bb.Max, bg);
  }
  const float glyph_size = std::floor(bb.GetHeight() * 0.36f);
  draw_glyph(dl, kind, bb.GetCenter(), glyph_size, text);
  return pressed;
}

// A 1px border just outside the window, then a soft shadow around it. Kept
// outside the window rectangle: windows docked inside a frame are drawn
// after it and would cover anything drawn along its inner edge.
void draw_border_and_shadow(ImGuiWindow* window, bool highlighted) {
  ImDrawList* dl = window->DrawList;
  const ImGuiViewport* vp = ImGui::GetWindowViewport();
  dl->PushClipRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), false);
  const ImVec2 min = window->Pos;
  const ImVec2 max = ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
  const float rounding = window->WindowRounding;
  constexpr int kShadowAlpha[] = {44, 26, 14, 6};
  for (int i = 0; i < 4; ++i) {
    const float d = 2.0f + float(i);
    dl->AddRect(ImVec2(min.x - d, min.y - d + 1.0f), ImVec2(max.x + d, max.y + d + 1.0f),
        IM_COL32(0, 0, 0, kShadowAlpha[i]), rounding > 0.0f ? rounding + d : 0.0f, 0, 1.0f);
  }
  ImVec4 border = ImGui::GetStyleColorVec4(highlighted ? ImGuiCol_TitleBgActive : ImGuiCol_Border);
  border.w = 1.0f;
  dl->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), ImGui::GetColorU32(border),
      rounding > 0.0f ? rounding + 1.0f : 0.0f, 0, 1.0f);
  dl->PopClipRect();
}

} // namespace

bool window_was_docked(const char* label) {
  const ImGuiWindow* window = ImGui::FindWindowByName(label);
  return window && window->DockIsActive;
}

caption_action draw_window_caption(const char* title, const caption_options& opts) {
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->DockIsActive || window->Collapsed || (window->Flags & ImGuiWindowFlags_NoTitleBar))
    return caption_action::none;

  const bool highlighted = title_is_highlighted(window);
  draw_border_and_shadow(window, highlighted);

  const ImGuiStyle& style = ImGui::GetStyle();
  const ImRect title_bar = window->TitleBarRect();
  const float h = title_bar.GetHeight();
  const float w = std::floor(h * 1.5f);

  // The title bar lies outside the content clip rectangle.
  ImGui::PushClipRect(title_bar.Min, title_bar.Max, false);
  const ImGuiNavLayer layer_backup = window->DC.NavLayerCurrent;
  window->DC.NavLayerCurrent = ImGuiNavLayer_Menu;

  caption_action action = caption_action::none;
  bool any_hovered = false;
  ImRect bb(ImVec2(title_bar.Max.x - w, title_bar.Min.y), title_bar.Max);
  if (caption_button(window, "#wish_caption_close", bb, glyph::close, opts.can_close, any_hovered))
    action = caption_action::close;
  bb.TranslateX(-w);
  if (caption_button(window, "#wish_caption_max", bb, opts.maximized ? glyph::restore : glyph::maximize,
          opts.can_maximize, any_hovered))
    action = caption_action::toggle_maximize;
  bb.TranslateX(-w);
  if (caption_button(window, "#wish_caption_min", bb, glyph::minimize, true, any_hovered))
    action = caption_action::minimize;

  // Title, ellipsized before the buttons.
  const ImVec2 text_min(title_bar.Min.x + style.FramePadding.x, title_bar.Min.y + style.FramePadding.y);
  const ImVec2 text_max(bb.Min.x - style.ItemInnerSpacing.x, title_bar.Max.y);
  if (title && *title && text_max.x > text_min.x)
    ImGui::RenderTextEllipsis(window->DrawList, text_min, text_max, text_max.x, title, nullptr, nullptr);

  window->DC.NavLayerCurrent = layer_backup;
  ImGui::PopClipRect();

  // Double-click on the title toggles maximize, as in Windows.
  if (action == caption_action::none && opts.can_maximize && !any_hovered && GImGui->HoveredWindow == window &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && title_bar.Contains(ImGui::GetIO().MousePos))
    action = caption_action::toggle_maximize;
  return action;
}

} // namespace bdg::wish
