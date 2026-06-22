// MIT License © 2025 Binary Dice Games
/// @file plot_histogram.cpp
/// @brief Registers PlotHistogram and PlotHistogram2D prototypes.
#include "src/bison/bison_object.hpp"

#include "plot_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_plot_histogram() {
  // PlotHistogram — 1-D frequency histogram.
  // bins: -1 = Sturges, -2 = Scott, -3 = Rice, -4 = Sqrt, >0 = explicit count.
  {
    auto proto = dynamic_ptr{"PlotHistogram"_key, {}};
    proto->addField("values"_key, field{std::vector<float>{},
      attr<DisplayName>("Values"),
      attr<Description>("Array of sample values to bin into the histogram."),
      attr<Category>("Data")});
    proto->addField("bins"_key, field{int32_t{-1},
      attr<DisplayName>("Bins"),
      attr<Description>(
          "Number of bins.  "
          "-1 = Sturges, -2 = Scott, -3 = Rice, -4 = Sqrt, >0 = explicit count."),
      attr<Category>("Behavior")});
    proto->addField("cumulative"_key, field{false,
      attr<DisplayName>("Cumulative"),
      attr<Description>("When true, renders a cumulative distribution instead."),
      attr<Category>("Behavior")});
    proto->addField("density"_key, field{false,
      attr<DisplayName>("Density"),
      attr<Description>("When true, normalizes bar heights to form a probability density."),
      attr<Category>("Behavior")});
    proto->addField("range_min"_key, field{float{0.0f},
      attr<DisplayName>("Range Min"),
      attr<Description>(
          "Minimum sample value to include.  "
          "When range_min == range_max (default 0/0), ImPlot auto-ranges."),
      attr<Category>("Behavior")});
    proto->addField("range_max"_key, field{float{0.0f},
      attr<DisplayName>("Range Max"),
      attr<Description>(
          "Maximum sample value to include.  "
          "When range_min == range_max (default 0/0), ImPlot auto-ranges."),
      attr<Category>("Behavior")});
    dynamic::addClass("wish"_key, std::move(proto), "PlotItem"_key, {
      attr<DisplayName>("PlotHistogram"),
      attr<Description>(
          "Draws a 1-D frequency histogram of the values array.  "
          "Must be a child of a Plot element.")});
  }

  // PlotHistogram2D — 2-D frequency heatmap.
  {
    auto proto = dynamic_ptr{"PlotHistogram2D"_key, {}};
    proto->addField("xs"_key, field{std::vector<float>{},
      attr<DisplayName>("X Values"),
      attr<Description>("Array of sample X coordinates."),
      attr<Category>("Data")});
    proto->addField("ys"_key, field{std::vector<float>{},
      attr<DisplayName>("Y Values"),
      attr<Description>("Array of sample Y coordinates (must match xs in length)."),
      attr<Category>("Data")});
    proto->addField("x_bins"_key, field{int32_t{-1},
      attr<DisplayName>("X Bins"),
      attr<Description>("Bin count for X; same sentinel values as PlotHistogram.bins."),
      attr<Category>("Behavior")});
    proto->addField("y_bins"_key, field{int32_t{-1},
      attr<DisplayName>("Y Bins"),
      attr<Description>("Bin count for Y; same sentinel values as PlotHistogram.bins."),
      attr<Category>("Behavior")});
    dynamic::addClass("wish"_key, std::move(proto), "PlotItem"_key, {
      attr<DisplayName>("PlotHistogram2D"),
      attr<Description>(
          "Draws a 2-D frequency heatmap of the (xs, ys) sample pairs.  "
          "Must be a child of a Plot element.")});
  }
}

}  // namespace bdg::wish
