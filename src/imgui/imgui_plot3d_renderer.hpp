// MIT License © 2025 Binary Dice Games
/// @file imgui_plot3d_renderer.hpp
/// @brief Declares ImPlot3D render functions called from imgui_renderer's dispatch table.
///
/// Functions are defined in imgui_plot3d_renderer.cpp and share the uniform
/// signature used throughout the imgui_renderer dispatch table:
///   void(imgui_renderer&, const ui_element&, const session&)
#pragma once

#ifdef WISH_IMGUI_ENABLED

#include <wish/imgui_renderer.hpp>
#include <wish/ui_element.hpp>
#include <wish/session.hpp>

namespace bdg::wish {

void render_plot3d(imgui_renderer& r, const ui_element& node, const session& s);

void render_plot3d_line(imgui_renderer&, const ui_element& node, const session&);
void render_plot3d_scatter(imgui_renderer&, const ui_element& node, const session&);

void render_plot3d_surface(imgui_renderer&, const ui_element& node, const session&);

void render_plot3d_triangle(imgui_renderer&, const ui_element& node, const session&);
void render_plot3d_quad(imgui_renderer&, const ui_element& node, const session&);
void render_plot3d_mesh(imgui_renderer&, const ui_element& node, const session&);

void render_plot3d_text(imgui_renderer&, const ui_element& node, const session&);

}  // namespace bdg::wish

#endif  // WISH_IMGUI_ENABLED
