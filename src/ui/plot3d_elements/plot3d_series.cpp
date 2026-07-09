// MIT License © 2025 Binary Dice Games
/// @file plot3d_series.cpp
/// @brief Registers line and scatter 3-D series prototypes.
///
/// Covered classes: Plot3DLine, Plot3DScatter.
/// Both inherit Plot3DItem and take xs / ys / zs data arrays.
#include "src/bison/bison_object.hpp"

#include "plot3d_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

static void add_xyz_fields(dynamic_ptr& proto) {
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
  proto->addField(
      "zs"_key,
      field{
          std::vector<float>{},
          attr<DisplayName>("Z Values"),
          attr<Description>("Array of Z coordinates, one per data point."),
          attr<Category>("Data")});
}

void register_plot3d_series() {
  // Plot3DLine — a connected 3-D line series.
  {
    auto proto = dynamic_ptr{"Plot3DLine"_key, {}};
    add_xyz_fields(proto);
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot3DLine"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws a connected 3-D line through (xs[i], ys[i], zs[i]) data points. "
                          "Must be a child of a Plot3D element."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Plot3DItem"_key,
        dynamic::make_factory<ui_element>("wish"_key, "Plot3DLine"_key));
  }

  // Plot3DScatter — dots at each 3-D data point, no connecting line.
  {
    auto proto = dynamic_ptr{"Plot3DScatter"_key, {}};
    add_xyz_fields(proto);
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot3DScatter"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws a marker at each (xs[i], ys[i], zs[i]) without connecting lines. "
                          "Must be a child of a Plot3D element."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Plot3DItem"_key,
        dynamic::make_factory<ui_element>("wish"_key, "Plot3DScatter"_key));
  }
}

} // namespace bdg::wish
