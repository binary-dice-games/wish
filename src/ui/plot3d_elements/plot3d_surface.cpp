// MIT License © 2025 Binary Dice Games
/// @file plot3d_surface.cpp
/// @brief Registers Plot3DSurface prototype.
#include "src/bison/bison_object.hpp"

#include "plot3d_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_plot3d_surface() {
  // Plot3DSurface — a 3-D parametric surface defined on an x_count × y_count grid.
  // xs, ys, zs are flattened row-major arrays of x_count * y_count elements.
  {
    auto proto = dynamic_ptr{"Plot3DSurface"_rkey, {}};
    proto->addField(
        "xs"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("X Grid"),
            attr<Description>("Flattened x_count × y_count array of X coordinates for each grid point."),
            attr<Category>("Data")});
    proto->addField(
        "ys"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("Y Grid"),
            attr<Description>("Flattened x_count × y_count array of Y coordinates for each grid point."),
            attr<Category>("Data")});
    proto->addField(
        "zs"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("Z Grid"),
            attr<Description>("Flattened x_count × y_count array of Z (height) values."),
            attr<Category>("Data")});
    proto->addField(
        "x_count"_rkey,
        field{
            int32_t{2},
            attr<DisplayName>("X Count"),
            attr<Description>("Number of grid points along the X (first) dimension."),
            attr<Category>("Data"),
            attr<Range>(2.0, 4096.0)});
    proto->addField(
        "y_count"_rkey,
        field{
            int32_t{2},
            attr<DisplayName>("Y Count"),
            attr<Description>("Number of grid points along the Y (second) dimension."),
            attr<Category>("Data"),
            attr<Range>(2.0, 4096.0)});
    proto->addField(
        "scale_min"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Scale Min"),
            attr<Description>("Minimum value used to map Z height to colormap. "
                              "Both scale_min and scale_max equal to 0 triggers auto-range."),
            attr<Category>("Style")});
    proto->addField(
        "scale_max"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Scale Max"),
            attr<Description>("Maximum value used to map Z height to colormap. "
                              "Both scale_min and scale_max equal to 0 triggers auto-range."),
            attr<Category>("Style")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot3DSurface"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws a colored 3-D parametric surface on an x_count × y_count grid. "
                          "xs, ys, zs are flattened row-major arrays. "
                          "Must be a child of a Plot3D element."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Plot3DItem"_key,
        dynamic::make_factory<ui_plot3d_surface>("wish"_key, "Plot3DSurface"_key));
  }
}

} // namespace bdg::wish
