// MIT License © 2025 Binary Dice Games
/// @file plot3d_annotations.cpp
/// @brief Registers Plot3DText annotation prototype.
#include "src/bison/bison_object.hpp"

#include "plot3d_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_plot3d_annotations() {
  // Plot3DText — text annotation positioned at a 3-D plot coordinate.
  {
    auto proto = dynamic_ptr{"Plot3DText"_key, {}};
    proto->addField("text"_key, field{std::string{},
      attr<DisplayName>("Text"),
      attr<Description>("The string to display at the given 3-D position."),
      attr<Category>("Content")});
    proto->addField("x"_key, field{float{0.0f},
      attr<DisplayName>("X"),
      attr<Description>("Plot X coordinate of the text anchor."),
      attr<Category>("Position")});
    proto->addField("y"_key, field{float{0.0f},
      attr<DisplayName>("Y"),
      attr<Description>("Plot Y coordinate of the text anchor."),
      attr<Category>("Position")});
    proto->addField("z"_key, field{float{0.0f},
      attr<DisplayName>("Z"),
      attr<Description>("Plot Z coordinate of the text anchor."),
      attr<Category>("Position")});
    proto->addField("angle"_key, field{float{0.0f},
      attr<DisplayName>("Angle"),
      attr<Description>("Rotation angle of the text in degrees."),
      attr<Category>("Style"),
      attr<Range>(-360.0, 360.0)});
    proto->addField("offset_x"_key, field{float{0.0f},
      attr<DisplayName>("Offset X"),
      attr<Description>("Pixel offset applied to the text after projection (horizontal)."),
      attr<Category>("Position")});
    proto->addField("offset_y"_key, field{float{0.0f},
      attr<DisplayName>("Offset Y"),
      attr<Description>("Pixel offset applied to the text after projection (vertical)."),
      attr<Category>("Position")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot3DText"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "Renders a text string at a given 3-D plot coordinate. "
        "Must be a child of a Plot3D element."));
    dynamic::addClass("wish"_key, std::move(proto), "Plot3DItem"_key,
        dynamic::make_factory<ui_element>("wish"_key, "Plot3DText"_key));
  }
}

}  // namespace bdg::wish
