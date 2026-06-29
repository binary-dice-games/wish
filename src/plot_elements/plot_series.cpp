// MIT License © 2025 Binary Dice Games
/// @file plot_series.cpp
/// @brief Registers line-based and area plot series prototypes.
///
/// Covered classes: PlotLine, PlotScatter, PlotStairs, PlotStems,
///                  PlotShaded, PlotDigital.
/// All inherit PlotItem and share xs / ys data arrays.
#include "src/bison/bison_object.hpp"

#include "plot_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

// Shared helper: add the xs/ys array fields to a proto.
static void add_xy_fields(dynamic_ptr& proto) {
  proto->addField(
      "xs"_key,
      field{
          std::vector<float>{},
          attr<DisplayName>("X Values"),
          attr<Description>("Array of X coordinates, one per data point."),
          attr<Category>("Data")});
  proto->addField(
      "ys"_key,
      field{
          std::vector<float>{},
          attr<DisplayName>("Y Values"),
          attr<Description>("Array of Y coordinates, one per data point."),
          attr<Category>("Data")});
}

void register_plot_series() {
  // PlotLine — a connected line series.
  {
    auto proto = dynamic_ptr{"PlotLine"_key, {}};
    add_xy_fields(proto);
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotLine"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws a connected line through the (xs, ys) data points.  "
                          "Must be a child of a Plot element."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "PlotItem"_key, dynamic::make_factory<ui_element>("wish"_key, "PlotLine"_key));
  }

  // PlotScatter — dots at each data point, no connecting line.
  {
    auto proto = dynamic_ptr{"PlotScatter"_key, {}};
    add_xy_fields(proto);
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotScatter"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws a marker at each (xs[i], ys[i]) coordinate without connecting lines.  "
                          "Must be a child of a Plot element."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "PlotItem"_key, dynamic::make_factory<ui_element>("wish"_key, "PlotScatter"_key));
  }

  // PlotStairs — staircase / step interpolation between data points.
  {
    auto proto = dynamic_ptr{"PlotStairs"_key, {}};
    add_xy_fields(proto);
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotStairs"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws a staircase (step-interpolated) line through the data.  "
                          "Must be a child of a Plot element."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "PlotItem"_key, dynamic::make_factory<ui_element>("wish"_key, "PlotStairs"_key));
  }

  // PlotStems — vertical lines from a reference level to each data point.
  {
    auto proto = dynamic_ptr{"PlotStems"_key, {}};
    add_xy_fields(proto);
    proto->addField(
        "ref"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Reference"),
            attr<Description>("Y coordinate of the baseline from which stems grow."),
            attr<Category>("Data")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotStems"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws vertical stems from the reference level to each (xs[i], ys[i]).  "
                          "Must be a child of a Plot element."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "PlotItem"_key, dynamic::make_factory<ui_element>("wish"_key, "PlotStems"_key));
  }

  // PlotShaded — filled area between two curves (or between one curve and ref).
  {
    auto proto = dynamic_ptr{"PlotShaded"_key, {}};
    add_xy_fields(proto);
    proto->addField(
        "ys2"_key,
        field{
            std::vector<float>{},
            attr<DisplayName>("Y Values 2"),
            attr<Description>("Second Y array for a band between ys and ys2.  "
                              "Leave empty to shade between ys and ref."),
            attr<Category>("Data")});
    proto->addField(
        "ref"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Reference"),
            attr<Description>("Horizontal reference line used when ys2 is empty."),
            attr<Category>("Data")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotShaded"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws a shaded region between two curves, or between ys and a reference.  "
                          "Must be a child of a Plot element."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "PlotItem"_key, dynamic::make_factory<ui_element>("wish"_key, "PlotShaded"_key));
  }

  // PlotDigital — binary / digital signal display.
  {
    auto proto = dynamic_ptr{"PlotDigital"_key, {}};
    add_xy_fields(proto);
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PlotDigital"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws a digital signal trace: xs = timestamps, ys = 0 or 1.  "
                          "Must be a child of a Plot element."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "PlotItem"_key, dynamic::make_factory<ui_element>("wish"_key, "PlotDigital"_key));
  }
}

} // namespace bdg::wish
