// MIT License © 2025 Binary Dice Games
/// @file theme_classic.cpp
/// @brief Registers the "classic" theme (`ImGui::StyleColorsClassic`).
#include <imgui/imgui_renderer.hpp>
#include <imgui/themes/themes.hpp>

#include <imgui.h>

namespace bdg::wish {

void register_theme_classic() {
  register_theme("classic", ImGui::StyleColorsClassic);
}

} // namespace bdg::wish
