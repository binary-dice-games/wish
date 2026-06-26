// MIT License © 2025 Binary Dice Games
/// @file plot_pie.cpp
/// @brief Registers the PlotPieChart prototype.
///
/// Pie charts must be placed inside a Plot element whose axes are configured
/// with ImPlotAxisFlags_NoDecorations (0x1F) and fixed limits [0,1] x [0,1].
/// In the JSON descriptor set x_flags and y_flags to 31 on the parent Plot.
#include "src/bison/bison_object.hpp"

#include "plot_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_plot_pie() {
  auto proto = dynamic_ptr{"PlotPieChart"_key, {}};
  proto->addField("labels"_key, field{std::string{},
    attr<DisplayName>("Labels"),
    attr<Description>(
        "Newline-separated list of slice labels, "
        "one per entry in the values array."),
    attr<Category>("Content")});
  proto->addField("values"_key, field{std::vector<float>{},
    attr<DisplayName>("Values"),
    attr<Description>(
        "Array of slice magnitudes.  "
        "When normalize is false, values are used as-is; "
        "when true, they are normalized to sum to 1."),
    attr<Category>("Data")});
  proto->addField("x"_key, field{float{0.5f},
    attr<DisplayName>("Center X"),
    attr<Description>(
        "X coordinate of the pie center in plot (data) units [0, 1]."),
    attr<Category>("Layout"),
    attr<Range>(0.0, 1.0)});
  proto->addField("y"_key, field{float{0.5f},
    attr<DisplayName>("Center Y"),
    attr<Description>(
        "Y coordinate of the pie center in plot (data) units [0, 1]."),
    attr<Category>("Layout"),
    attr<Range>(0.0, 1.0)});
  proto->addField("radius"_key, field{float{0.4f},
    attr<DisplayName>("Radius"),
    attr<Description>("Pie radius in plot (data) units."),
    attr<Category>("Layout"),
    attr<Range>(0.0, 1.0)});
  proto->addField("normalize"_key, field{false,
    attr<DisplayName>("Normalize"),
    attr<Description>(
        "When true, slices are proportional to their values "
        "relative to the total sum."),
    attr<Category>("Behavior")});
  proto->addField("label_fmt"_key, field{std::string{"%.1f%%"},
    attr<DisplayName>("Label Format"),
    attr<Description>("printf format for the per-slice value label."),
    attr<Category>("Appearance")});
  proto->addField("angle0"_key, field{float{90.0f},
    attr<DisplayName>("Start Angle"),
    attr<Description>(
        "Angle in degrees at which the first slice begins (0° = 3 o'clock, "
        "90° = 12 o'clock)."),
    attr<Category>("Appearance"),
    attr<Range>(0.0, 360.0)});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotPieChart"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Draws a pie chart.  "
      "Place inside a Plot with x_flags=31 (NoDecorations) and y_flags=31, "
      "and set Plot axis limits to [0,1] x [0,1] for correct scaling."));
  dynamic::addClass("wish"_key, std::move(proto), "PlotItem"_key,
      dynamic::make_factory<ui_element>("wish"_key, "PlotPieChart"_key));
}

}  // namespace bdg::wish
