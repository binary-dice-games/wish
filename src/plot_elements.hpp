// MIT License © 2025 Binary Dice Games
/// @file plot_elements.hpp
/// @brief Internal declarations for per-plot-element registration functions.
///        Not part of the public wish API.
#pragma once

#include <wish/ui_element.hpp>

namespace bdg::wish {

void register_plot();            // Plot (container), PlotItem (series base)
void register_plot_series();     // PlotLine, PlotScatter, PlotStairs, PlotStems,
                                 // PlotShaded, PlotDigital
void register_plot_bars();       // PlotBars, PlotBarsH
void register_plot_histogram();  // PlotHistogram, PlotHistogram2D
void register_plot_heatmap();    // PlotHeatmap
void register_plot_pie();        // PlotPieChart
void register_plot_annotations();// PlotText, PlotInfLines

}  // namespace bdg::wish
