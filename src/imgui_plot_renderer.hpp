// MIT License © 2025 Binary Dice Games
/// @file imgui_plot_renderer.hpp
/// @brief Declares ImPlot render functions called from imgui_renderer's dispatch table.
///
/// Functions are defined in imgui_plot_renderer.cpp and have the same uniform
/// signature used throughout the imgui_renderer dispatch table:
///   void(imgui_renderer&, const ui_element&, session&)
#pragma once

#ifdef WISH_IMGUI_ENABLED

#include <wish/imgui_renderer.hpp>
#include <wish/ui_element.hpp>
#include <wish/session.hpp>

namespace bdg::wish {

void render_plot(imgui_renderer& r, const ui_element& node, session& s);

void render_plot_line(imgui_renderer&, const ui_element& node, session&);
void render_plot_scatter(imgui_renderer&, const ui_element& node, session&);
void render_plot_stairs(imgui_renderer&, const ui_element& node, session&);
void render_plot_stems(imgui_renderer&, const ui_element& node, session&);
void render_plot_shaded(imgui_renderer&, const ui_element& node, session&);
void render_plot_digital(imgui_renderer&, const ui_element& node, session&);

void render_plot_bars(imgui_renderer&, const ui_element& node, session&);
void render_plot_bars_h(imgui_renderer&, const ui_element& node, session&);

void render_plot_histogram(imgui_renderer&, const ui_element& node, session&);
void render_plot_histogram2d(imgui_renderer&, const ui_element& node, session&);

void render_plot_heatmap(imgui_renderer&, const ui_element& node, session&);

void render_plot_pie_chart(imgui_renderer&, const ui_element& node, session&);

void render_plot_text(imgui_renderer&, const ui_element& node, session&);
void render_plot_inf_lines(imgui_renderer&, const ui_element& node, session&);

}  // namespace bdg::wish

#endif  // WISH_IMGUI_ENABLED
