// MIT License © 2025 Binary Dice Games
/// @file theme_light.cpp
/// @brief Registers the "light" theme (`ImGui::StyleColorsLight`).
#include <imgui/imgui_renderer.hpp>
#include <imgui/themes/themes.hpp>

#include <imgui.h>

namespace bdg::wish {

void register_theme_light() {
  register_theme("light", ImGui::StyleColorsLight);
}

} // namespace bdg::wish
