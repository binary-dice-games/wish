// MIT License © 2026 Binary Dice Games
/// @file imgui_icon_widgets.hpp
/// @brief Button / MenuItem variants that draw an image icon next to their
///        label (the `icon` field of Button, MenuButton and MenuItem).
#pragma once

#include <imgui.h>

namespace bdg::wish {

/// @brief A framed button showing @p icon at text height, left of @p label.
///
/// Sized like `ImGui::Button(label, size)` with the icon (plus inner spacing)
/// added to the content width; an empty @p label gives a square, frame-height
/// icon-only button. The content is placed per `ImGuiStyle::ButtonTextAlign`.
/// The button is the last ImGui item afterwards, so tooltips / drag sources
/// attach to it as they would to a plain Button.
///
/// @param label  Caption; text after `##` is hidden, as in ImGui.
/// @param icon   Texture to draw; must be non-null.
/// @param tint   Color multiplied over the icon (usually the text color).
/// @param size   Same meaning as ImGui::Button()'s size (0 = auto, <0 = fill).
/// @return True on the frame the button is clicked.
bool icon_button(const char* label, ImTextureID icon, const ImVec4& tint, const ImVec2& size = ImVec2(0, 0));

/// @brief `ImGui::MenuItem()` with @p icon drawn in the popup's icon column.
///
/// Uses ImGui's own icon column (`MenuItemEx()`), so labels of all items in
/// the same popup stay aligned whether or not each has an icon. Inside a
/// horizontal menu bar the icon is ignored (ImGui has no icon column there).
/// A null @p icon behaves exactly like `ImGui::MenuItem()`.
///
/// @return True on the frame the item is activated.
bool icon_menu_item(const char* label, const char* shortcut, bool selected, bool enabled, ImTextureID icon);

} // namespace bdg::wish
