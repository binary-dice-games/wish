// MIT License © 2025 Binary Dice Games
/// @file theme_wish.cpp
/// @brief Registers the "wish" theme: `ImGui::StyleColorsLight` with a more
///        modern look layered on top -- wish's default theme.
#include <imgui/imgui_renderer.hpp>
#include <imgui/themes/themes.hpp>

#include <imgui.h>

namespace bdg::wish {

namespace {

// Softer corner rounding, roomier padding, and no inner frame borders.
void StyleColorsWish(ImGuiStyle* dst) {
  ImGuiStyle& style = dst ? *dst : ImGui::GetStyle();
  ImGui::StyleColorsLight(&style);

  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.GrabRounding = 4.0f;
  style.TabRounding = 4.0f;

  style.WindowPadding = ImVec2(12.0f, 12.0f);
  style.FramePadding = ImVec2(8.0f, 5.0f);
  style.ItemSpacing = ImVec2(8.0f, 6.0f);

  style.WindowBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
}

} // namespace

void register_theme_wish() {
  register_theme("wish", StyleColorsWish, /*is_light=*/true);
}

} // namespace bdg::wish
