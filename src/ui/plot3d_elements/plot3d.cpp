// MIT License © 2025 Binary Dice Games
/// @file plot3d.cpp
/// @brief Registers Plot3D (ImPlot3D container) and Plot3DItem (series base) prototypes.
#include "src/bison/bison_object.hpp"

#include "plot3d_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_plot3d() {
  // Plot3DItem — hidden base class for all series drawn inside a Plot3D element.
  // Carries only the legend label; concrete series types add their data arrays.
  {
    auto proto = dynamic_ptr{"Plot3DItem"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Series name shown in the plot legend."),
            attr<Category>("Content")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot3DItem"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Abstract base class for all 3-D plot series. "
                          "Must be placed as a direct child of a Plot3D element."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Plot3DItem"_key));
  }

  // Plot3D — ImPlot3D window container. Children should be Plot3DItem elements.
  {
    auto proto = dynamic_ptr{"Plot3D"_rkey, {}};
    proto->addField(
        "title"_rkey,
        field{
            std::string{"##plot3d"},
            attr<DisplayName>("Title"),
            attr<Description>("Plot title shown at the top; prefix ## to hide the title bar."),
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
        "z_label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Z Label"),
            attr<Description>("Z axis label; empty string = no label."),
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
            float{400.0f},
            attr<DisplayName>("Height"),
            attr<Description>("Plot height in pixels."),
            attr<Category>("Layout"),
            attr<Range>(16.0, 8192.0)});
    proto->addField(
        "flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImPlot3DFlags bit field (see implot3d.h)."),
            attr<Category>("Behavior")});
    proto->addField(
        "x_flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("X Axis Flags"),
            attr<Description>("ImPlot3DAxisFlags for the X axis."),
            attr<Category>("Axes")});
    proto->addField(
        "y_flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Y Axis Flags"),
            attr<Description>("ImPlot3DAxisFlags for the Y axis."),
            attr<Category>("Axes")});
    proto->addField(
        "z_flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Z Axis Flags"),
            attr<Description>("ImPlot3DAxisFlags for the Z axis."),
            attr<Category>("Axes")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot3D"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("An ImPlot3D plot window. "
                          "Children should be Plot3DItem elements (Plot3DLine, Plot3DSurface, etc.)."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Plot3D"_key));
  }
}

} // namespace bdg::wish
