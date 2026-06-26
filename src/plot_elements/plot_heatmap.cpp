// MIT License © 2025 Binary Dice Games
/// @file plot_heatmap.cpp
/// @brief Registers the PlotHeatmap prototype.
#include "src/bison/bison_object.hpp"

#include "plot_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_plot_heatmap() {
  auto proto = dynamic_ptr{"PlotHeatmap"_key, {}};
  proto->addField("values"_key, field{std::vector<float>{},
    attr<DisplayName>("Values"),
    attr<Description>(
        "Flattened row-major data array of size rows × cols."),
    attr<Category>("Data")});
  proto->addField("rows"_key, field{int32_t{1},
    attr<DisplayName>("Rows"),
    attr<Description>("Number of rows in the 2-D grid."),
    attr<Category>("Layout")});
  proto->addField("cols"_key, field{int32_t{1},
    attr<DisplayName>("Cols"),
    attr<Description>("Number of columns in the 2-D grid."),
    attr<Category>("Layout")});
  proto->addField("scale_min"_key, field{float{0.0f},
    attr<DisplayName>("Scale Min"),
    attr<Description>("Value mapped to the low end of the colormap."),
    attr<Category>("Appearance")});
  proto->addField("scale_max"_key, field{float{1.0f},
    attr<DisplayName>("Scale Max"),
    attr<Description>("Value mapped to the high end of the colormap."),
    attr<Category>("Appearance")});
  proto->addField("format"_key, field{std::string{"%.1f"},
    attr<DisplayName>("Label Format"),
    attr<Description>(
        "printf format for per-cell value labels; "
        "empty string disables cell labels."),
    attr<Category>("Appearance")});
  proto->addField("x_min"_key, field{float{0.0f},
    attr<DisplayName>("X Min (Bound)"),
    attr<Description>("X coordinate of the left edge of the heatmap bounds."),
    attr<Category>("Layout")});
  proto->addField("x_max"_key, field{float{1.0f},
    attr<DisplayName>("X Max (Bound)"),
    attr<Description>("X coordinate of the right edge of the heatmap bounds."),
    attr<Category>("Layout")});
  proto->addField("y_min"_key, field{float{0.0f},
    attr<DisplayName>("Y Min (Bound)"),
    attr<Description>("Y coordinate of the bottom edge of the heatmap bounds."),
    attr<Category>("Layout")});
  proto->addField("y_max"_key, field{float{1.0f},
    attr<DisplayName>("Y Max (Bound)"),
    attr<Description>("Y coordinate of the top edge of the heatmap bounds."),
    attr<Category>("Layout")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotHeatmap"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Draws a 2-D heatmap from a flat row-major values array.  "
      "Must be a child of a Plot element with axes configured to match bounds."));
  dynamic::addClass("wish"_key, std::move(proto), "PlotItem"_key,
      dynamic::make_factory<ui_element>("wish"_key, "PlotHeatmap"_key));
}

}  // namespace bdg::wish
