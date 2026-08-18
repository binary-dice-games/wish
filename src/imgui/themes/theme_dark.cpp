// MIT License © 2025 Binary Dice Games
/// @file theme_dark.cpp
/// @brief Registers the "dark" theme (`ImGui::StyleColorsDark`).
#include <imgui/imgui_renderer.hpp>
#include <imgui/themes/themes.hpp>

#include <imgui.h>

namespace bdg::wish {

void register_theme_dark() {
  register_theme("dark", ImGui::StyleColorsDark);
}

} // namespace bdg::wish
