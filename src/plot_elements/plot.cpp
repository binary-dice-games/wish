// MIT License © 2025 Binary Dice Games
/// @file plot.cpp
/// @brief Registers Plot (ImPlot container) and PlotItem (series base) prototypes.
#include "src/bison/bison_object.hpp"

#include "plot_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_plot() {
  // PlotItem — hidden base class for all series drawn inside a Plot element.
  // Carries only the legend label; concrete series types add their data arrays.
  {
    auto proto = dynamic_ptr{"PlotItem"_key, {}};
    proto->addField(
        "label"_key,
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
    auto proto = dynamic_ptr{"Plot"_key, {}};
    proto->addField(
        "title"_key,
        field{
            std::string{"##plot"},
            attr<DisplayName>("Title"),
            attr<Description>("Plot title shown at the top of the frame; "
                              "prefix ## to make the ID hidden (no visible title)."),
            attr<Category>("Content")});
    proto->addField(
        "x_label"_key,
        field{
            std::string{},
            attr<DisplayName>("X Label"),
            attr<Description>("X axis label; empty string = no label."),
            attr<Category>("Axes")});
    proto->addField(
        "y_label"_key,
        field{
            std::string{},
            attr<DisplayName>("Y Label"),
            attr<Description>("Y axis label; empty string = no label."),
            attr<Category>("Axes")});
    proto->addField(
        "width"_key,
        field{
            float{-1.0f},
            attr<DisplayName>("Width"),
            attr<Description>("Plot width in pixels; -1 fills the available horizontal space."),
            attr<Category>("Layout"),
            attr<Range>(-1.0, 8192.0)});
    proto->addField(
        "height"_key,
        field{
            float{300.0f},
            attr<DisplayName>("Height"),
            attr<Description>("Plot height in pixels."),
            attr<Category>("Layout"),
            attr<Range>(16.0, 8192.0)});
    proto->addField(
        "flags"_key,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImPlotFlags bit field (see implot.h)."),
            attr<Category>("Behavior")});
    proto->addField(
        "x_flags"_key,
        field{
            int32_t{0},
            attr<DisplayName>("X Axis Flags"),
            attr<Description>("ImPlotAxisFlags for the primary X axis.  "
                              "Use ImPlotAxisFlags_NoDecorations (0x1F) for pie charts."),
            attr<Category>("Axes")});
    proto->addField(
        "y_flags"_key,
        field{
            int32_t{0},
            attr<DisplayName>("Y Axis Flags"),
            attr<Description>("ImPlotAxisFlags for the primary Y axis.  "
                              "Use ImPlotAxisFlags_NoDecorations (0x1F) for pie charts."),
            attr<Category>("Axes")});
    proto->addField(
        "x_min"_key,
        field{
            float{0.0f},
            attr<DisplayName>("X Min"),
            attr<Description>("Fixed lower X-axis limit, locked every frame.  "
                              "Ignored (axis auto-fits normally) when equal to x_max."),
            attr<Category>("Axes")});
    proto->addField(
        "x_max"_key,
        field{
            float{0.0f},
            attr<DisplayName>("X Max"),
            attr<Description>("Fixed upper X-axis limit, locked every frame.  "
                              "Ignored (axis auto-fits normally) when equal to x_min."),
            attr<Category>("Axes")});
    proto->addField(
        "y_min"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Y Min"),
            attr<Description>("Fixed lower Y-axis limit, locked every frame.  "
                              "Ignored (axis auto-fits normally) when equal to y_max."),
            attr<Category>("Axes")});
    proto->addField(
        "y_max"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Y Max"),
            attr<Description>("Fixed upper Y-axis limit, locked every frame.  "
                              "Ignored (axis auto-fits normally) when equal to y_min."),
            attr<Category>("Axes")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("An ImPlot plot window.  "
                          "Children should be PlotItem elements (PlotLine, PlotBars, etc.)."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Plot"_key));
  }
}

} // namespace bdg::wish
