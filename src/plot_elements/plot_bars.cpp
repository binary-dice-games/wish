// MIT License © 2025 Binary Dice Games
/// @file plot_bars.cpp
/// @brief Registers PlotBars (vertical) and PlotBarsH (horizontal) prototypes.
#include "src/bison/bison_object.hpp"

#include "plot_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

static void add_bar_fields(dynamic_ptr& proto) {
  proto->addField("xs"_key, field{std::vector<float>{},
    attr<DisplayName>("X Positions"),
    attr<Description>(
        "Array of bar X positions.  "
        "If empty, bars are placed at 0, 1, 2, … using the ys values only."),
    attr<Category>("Data")});
  proto->addField("ys"_key, field{std::vector<float>{},
    attr<DisplayName>("Y Values"),
    attr<Description>("Array of bar heights (or lengths for horizontal bars)."),
    attr<Category>("Data")});
  proto->addField("bar_size"_key, field{float{0.67f},
    attr<DisplayName>("Bar Size"),
    attr<Description>("Width (or height for horizontal) of each bar in plot units."),
    attr<Category>("Appearance"),
    attr<Range>(0.0, 16.0)});
}

void register_plot_bars() {
  // PlotBars — vertical bar chart.
  {
    auto proto = dynamic_ptr{"PlotBars"_key, {}};
    add_bar_fields(proto);
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotBars"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "Draws vertical bars at each xs[i] with height ys[i].  "
        "If xs is empty, bars are placed at integer positions 0, 1, 2, …  "
        "Must be a child of a Plot element."));
    dynamic::addClass("wish"_key, std::move(proto), "PlotItem"_key,
        dynamic::make_factory<ui_element>("wish"_key, "PlotBars"_key));
  }

  // PlotBarsH — horizontal bar chart.
  {
    auto proto = dynamic_ptr{"PlotBarsH"_key, {}};
    add_bar_fields(proto);
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotBarsH"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "Draws horizontal bars at each ys[i] with length xs[i].  "
        "Implemented via ImPlot::PlotBars with ImPlotBarsFlags_Horizontal.  "
        "Must be a child of a Plot element."));
    dynamic::addClass("wish"_key, std::move(proto), "PlotItem"_key,
        dynamic::make_factory<ui_element>("wish"_key, "PlotBarsH"_key));
  }
}

}  // namespace bdg::wish
