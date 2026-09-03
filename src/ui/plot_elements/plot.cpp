// MIT License © 2025 Binary Dice Games
/// @file plot.cpp
/// @brief Registers Plot (ImPlot container) and PlotItem (series base) prototypes.
#include "src/bison/bison_object.hpp"

#include "plot_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

namespace {

EnumFlags::table plot_flags_table() {
  return {
      {"NoTitle", 1 << 0},
      {"NoLegend", 1 << 1},
      {"NoMouseText", 1 << 2},
      {"NoInputs", 1 << 3},
      {"NoMenus", 1 << 4},
      {"NoBoxSelect", 1 << 5},
      {"NoFrame", 1 << 6},
      {"Equal", 1 << 7},
      {"Crosshairs", 1 << 8},
      // Convenience composite — listed after single-bit flags so format()
      // prefers the fine-grained names when decomposing a value.
      {"CanvasOnly", (1 << 0) | (1 << 1) | (1 << 4) | (1 << 5) | (1 << 2)},
  };
}

// Shared between Plot.x_flags and Plot.y_flags -- both map to ImPlotAxisFlags.
EnumFlags::table plot_axis_flags_table() {
  return {
      {"NoLabel", 1 << 0},
      {"NoGridLines", 1 << 1},
      {"NoTickMarks", 1 << 2},
      {"NoTickLabels", 1 << 3},
      {"NoInitialFit", 1 << 4},
      {"NoMenus", 1 << 5},
      {"NoSideSwitch", 1 << 6},
      {"NoHighlight", 1 << 7},
      {"Opposite", 1 << 8},
      {"Foreground", 1 << 9},
      {"Invert", 1 << 10},
      {"AutoFit", 1 << 11},
      {"RangeFit", 1 << 12},
      {"PanStretch", 1 << 13},
      {"LockMin", 1 << 14},
      {"LockMax", 1 << 15},
      // Convenience composites — listed after single-bit flags so format()
      // prefers the fine-grained names when decomposing a value.
      {"Lock", (1 << 14) | (1 << 15)},
      {"NoDecorations", (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3)},
  };
}

// Plot.legend_location -> ImPlotLocation (a single choice, not a bitmask, but
// the compass points are OR-able so an EnumFlags table renders them cleanly).
EnumFlags::table plot_legend_location_table() {
  return {
      {"Center", 0},
      {"North", 1 << 0},
      {"South", 1 << 1},
      {"West", 1 << 2},
      {"East", 1 << 3},
      {"NorthWest", (1 << 0) | (1 << 2)},
      {"NorthEast", (1 << 0) | (1 << 3)},
      {"SouthWest", (1 << 1) | (1 << 2)},
      {"SouthEast", (1 << 1) | (1 << 3)},
  };
}

// Plot.legend_flags -> ImPlotLegendFlags.
EnumFlags::table plot_legend_flags_table() {
  return {
      {"NoButtons", 1 << 0},
      {"NoHighlightItem", 1 << 1},
      {"NoHighlightAxis", 1 << 2},
      {"NoMenus", 1 << 3},
      {"Outside", 1 << 4},
      {"Horizontal", 1 << 5},
      {"Sort", 1 << 6},
      {"Reverse", 1 << 7},
  };
}

} // namespace

void register_plot() {
  // PlotItem — hidden base class for all series drawn inside a Plot element.
  // Carries only the legend label; concrete series types add their data arrays.
  {
    auto proto = dynamic_ptr{"PlotItem"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Series name shown in the plot legend."),
            attr<Category>("Content")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotItem"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Abstract base class for all plot series. "
                          "Must be placed as a direct child of a Plot element."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "PlotItem"_key));
  }

  // Plot — ImPlot window container.  Children should be PlotItem elements.
  {
    auto proto = dynamic_ptr{"Plot"_rkey, {}};
    proto->addField(
        "title"_rkey,
        field{
            std::string{"##plot"},
            attr<DisplayName>("Title"),
            attr<Description>("Plot title shown at the top of the frame; "
                              "prefix ## to make the ID hidden (no visible title)."),
            attr<Category>("Content")});
    proto->addField(
        "x_label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("X Label"),
            attr<Description>("X axis label; empty string = no label."),
            attr<Category>("Axes")});
    proto->addField(
        "y_label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Y Label"),
            attr<Description>("Y axis label; empty string = no label."),
            attr<Category>("Axes")});
    proto->addField(
        "width"_rkey,
        field{
            float{-1.0f},
            attr<DisplayName>("Width"),
            attr<Description>("Plot width in pixels; -1 fills the available horizontal space."),
            attr<Category>("Layout"),
            attr<Range>(-1.0, 8192.0)});
    proto->addField(
        "height"_rkey,
        field{
            float{300.0f},
            attr<DisplayName>("Height"),
            attr<Description>("Plot height in pixels."),
            attr<Category>("Layout"),
            attr<Range>(16.0, 8192.0)});
    proto->addField(
        "flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImPlotFlags bitmask (combine names with '|'; see implot.h)."),
            attr<Category>("Behavior"),
            attr<EnumFlags>(plot_flags_table())});
    proto->addField(
        "x_flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("X Axis Flags"),
            attr<Description>("ImPlotAxisFlags for the primary X axis (combine names with '|'). "
                              "Use NoDecorations for pie charts."),
            attr<Category>("Axes"),
            attr<EnumFlags>(plot_axis_flags_table())});
    proto->addField(
        "y_flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Y Axis Flags"),
            attr<Description>("ImPlotAxisFlags for the primary Y axis (combine names with '|'). "
                              "Use NoDecorations for pie charts."),
            attr<Category>("Axes"),
            attr<EnumFlags>(plot_axis_flags_table())});
    proto->addField(
        "x_min"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("X Min"),
            attr<Description>("Fixed lower X-axis limit, locked every frame.  "
                              "Ignored (axis auto-fits normally) when equal to x_max."),
            attr<Category>("Axes")});
    proto->addField(
        "x_max"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("X Max"),
            attr<Description>("Fixed upper X-axis limit, locked every frame.  "
                              "Ignored (axis auto-fits normally) when equal to x_min."),
            attr<Category>("Axes")});
    proto->addField(
        "y_min"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Y Min"),
            attr<Description>("Fixed lower Y-axis limit, locked every frame.  "
                              "Ignored (axis auto-fits normally) when equal to y_max."),
            attr<Category>("Axes")});
    proto->addField(
        "y_max"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Y Max"),
            attr<Description>("Fixed upper Y-axis limit, locked every frame.  "
                              "Ignored (axis auto-fits normally) when equal to y_min."),
            attr<Category>("Axes")});
    proto->addField(
        "legend_location"_rkey,
        field{
            int32_t{-1},
            attr<DisplayName>("Legend Location"),
            attr<Description>("Where the legend sits (ImPlotLocation: North/South/West/East and the "
                              "corner combinations, or Center). -1 (default) keeps ImPlot's own "
                              "default of top-left inside the plot. Combine with legend_flags Outside "
                              "to move the legend out of the plot area entirely."),
            attr<Category>("Legend"),
            attr<EnumFlags>(plot_legend_location_table())});
    proto->addField(
        "legend_flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Legend Flags"),
            attr<Description>("ImPlotLegendFlags bitmask (combine names with '|'): Outside renders the "
                              "legend outside the plot frame, Horizontal lays entries in a row, plus "
                              "NoButtons / NoMenus / Sort / Reverse."),
            attr<Category>("Legend"),
            attr<EnumFlags>(plot_legend_flags_table())});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("An ImPlot plot window.  "
                          "Children should be PlotItem elements (PlotLine, PlotBars, etc.)."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_plot>("wish"_key, "Plot"_key));
  }
}

} // namespace bdg::wish
