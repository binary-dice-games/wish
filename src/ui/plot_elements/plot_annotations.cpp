// MIT License © 2025 Binary Dice Games
/// @file plot_annotations.cpp
/// @brief Registers PlotText and PlotInfLines prototypes.
#include "src/bison/bison_object.hpp"

#include "plot_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_plot_annotations() {
  // PlotText — a text annotation at a specific plot coordinate.
  {
    auto proto = dynamic_ptr{"PlotText"_key, {}};
    proto->addField(
        "text"_key,
        field{
            std::string{},
            attr<DisplayName>("Text"),
            attr<Description>("The string to display at the (x, y) plot coordinate."),
            attr<Category>("Content")});
    proto->addField(
        "x"_key,
        field{
            float{0.0f},
            attr<DisplayName>("X"),
            attr<Description>("X coordinate in plot (data) space."),
            attr<Category>("Layout")});
    proto->addField(
        "y"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Y"),
            attr<Description>("Y coordinate in plot (data) space."),
            attr<Category>("Layout")});
    proto->addField(
        "offset_x"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Pixel Offset X"),
            attr<Description>("Horizontal pixel offset applied after coordinate projection."),
            attr<Category>("Layout")});
    proto->addField(
        "offset_y"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Pixel Offset Y"),
            attr<Description>("Vertical pixel offset applied after coordinate projection."),
            attr<Category>("Layout")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotText"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws a text string at a plot coordinate.  "
                          "Must be a child of a Plot element."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "PlotItem"_key, dynamic::make_factory<ui_element>("wish"_key, "PlotText"_key));
  }

  // PlotInfLines — infinite vertical or horizontal reference lines.
  {
    auto proto = dynamic_ptr{"PlotInfLines"_key, {}};
    proto->addField(
        "values"_key,
        field{
            std::vector<float>{},
            attr<DisplayName>("Values"),
            attr<Description>("Array of axis positions at which to draw lines.  "
                              "When horizontal is false these are X positions (vertical lines); "
                              "when horizontal is true these are Y positions (horizontal lines)."),
            attr<Category>("Data")});
    proto->addField(
        "horizontal"_key,
        field{
            false,
            attr<DisplayName>("Horizontal"),
            attr<Description>("When false, draws vertical lines at each X value.  "
                              "When true, draws horizontal lines at each Y value."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotInfLines"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws infinite reference lines across the plot.  "
                          "Must be a child of a Plot element."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "PlotItem"_key,
        dynamic::make_factory<ui_element>("wish"_key, "PlotInfLines"_key));
  }
}

} // namespace bdg::wish
