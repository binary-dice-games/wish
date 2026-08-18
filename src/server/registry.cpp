// MIT License © 2025 Binary Dice Games
/// @file registry.cpp
#include <context/file_service.hpp>
#include <context/logger.hpp>
#include <server/registry.hpp>
#include <context/style_service.hpp>

#ifdef WISH_AUTOMATION_ENABLED
#include <automation/automation_service.hpp>
#endif

#include "ui/forms/file_dialog.hpp"
#include "ui/forms/message_box.hpp"
#include "ui/ui_elements/object_inspector.hpp"
#include "ui/plot3d_elements/plot3d_elements.hpp"
#include "ui/plot_elements/plot_elements.hpp"
#include "ui/ui_template.hpp"
#include "ui/ui_elements/ui_elements.hpp"
#include "server/wish_module_registry.hpp"

namespace bdg::wish {

void register_all() {
  // UI element classes — order matters: parents before children.
  register_element(); // root base: visible, children
  register_layout(); // Layout < Element; VerticalLayout, HorizontalLayout < Layout
  register_splitter(); // Splitter < Layout
  register_spring(); // Spring < Element
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
  register_graph_node();
  register_color_edit();
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
  register_ui_template();
  register_file_service();
  register_style_service();
  register_logger();
#ifdef WISH_AUTOMATION_ENABLED
  register_automation_service();
#endif
  // Built-in forms.
  register_file_dialog();
  register_message_box();
  // ObjectInspector: a real ui_element (unlike the elements above, it needs
  // its own C++ class -- see object_inspector.hpp), so registered here
  // alongside the forms rather than in the plain per-file register_*()
  // block earlier in this function.
  register_object_inspector();
  // Optional modules (calculator, notepad, process_explorer, ...); see
  // src/ui/forms/DESIGN.md's "Module System" section.
  register_optional_modules();
}

} // namespace bdg::wish
