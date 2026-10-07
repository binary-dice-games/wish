// MIT License © 2026 Binary Dice Games
/// @file imgui_treemap_renderer.hpp
/// @brief Declares the Treemap render function called from imgui_renderer's dispatch table.
///
/// Defined in imgui_treemap_renderer.cpp and shares the uniform signature
/// used throughout the imgui_renderer dispatch table:
///   void(imgui_renderer&, const ui_element&, const context&)
#pragma once

#ifdef WISH_IMGUI_ENABLED

#include "imgui_renderer.hpp"
#include "context/context.hpp"
#include "ui/ui_element.hpp"

namespace bdg::wish {

void render_treemap(imgui_renderer&, const ui_element& node, const context&);

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
