// MIT License © 2025 Binary Dice Games
/// @file registry.cpp
#include <file_service.hpp>
#include <logger.hpp>
#include <registry.hpp>
#include <style_service.hpp>

#include "forms/calculator.hpp"
#include "forms/file_dialog.hpp"
#include "forms/notepad.hpp"
#include "plot3d_elements/plot3d_elements.hpp"
#include "plot_elements/plot_elements.hpp"
#include "template_handler.hpp"
#include "ui_elements/ui_elements.hpp"
#ifdef WISH_MODULE_DESKTOP
#include "forms/desktop/desktop_module.hpp"
#endif
#ifdef WISH_MODULE_PROCESS_EXPLORER
#include "forms/process_explorer/process_explorer_module.hpp"
#endif

namespace bdg::wish {

void register_all() {
  // UI element classes — order matters: parents before children.
  register_element(); // root base: visible, children
  register_layout(); // Layout < Element; VerticalLayout, HorizontalLayout < Layout
  register_window();
  register_label();
  register_button();
  register_checkbox();
  register_slider(); // SliderFloat and SliderInt
  register_input_text();
  register_image();
  register_separator();
  register_menu();
  register_tabs();
  register_tree();
  register_combo();
  register_radio_button();
  register_progress_bar();
  register_input_number();
  register_drag();
  register_selectable();
  register_docking();
  register_table();
  register_text_editor();
  // Plot elements — must come after Element is registered.
  register_plot(); // PlotItem, Plot
  register_plot_series(); // PlotLine, PlotScatter, PlotStairs, PlotStems, PlotShaded, PlotDigital
  register_plot_bars(); // PlotBars, PlotBarsH
  register_plot_histogram(); // PlotHistogram, PlotHistogram2D
  register_plot_heatmap(); // PlotHeatmap
  register_plot_pie(); // PlotPieChart
  register_plot_annotations(); // PlotText, PlotInfLines
  // 3-D plot elements — must come after Element is registered.
  register_plot3d(); // Plot3DItem, Plot3D
  register_plot3d_series(); // Plot3DLine, Plot3DScatter
  register_plot3d_surface(); // Plot3DSurface
  register_plot3d_mesh(); // Plot3DTriangle, Plot3DQuad, Plot3DMesh
  register_plot3d_annotations(); // Plot3DText
  // Protocol handler classes.
  register_template_handler();
  register_file_service();
  register_style_service();
  register_logger();
  // Built-in forms.
  register_file_dialog();
  register_calculator();
  register_notepad();
#ifdef WISH_MODULE_DESKTOP
  register_desktop_module();
#endif
#ifdef WISH_MODULE_PROCESS_EXPLORER
  register_process_explorer_module();
#endif
}

} // namespace bdg::wish
