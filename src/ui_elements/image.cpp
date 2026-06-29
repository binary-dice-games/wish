// MIT License © 2025 Binary Dice Games
/// @file image.cpp
/// @brief Registers the Image prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_image() {
  auto proto = dynamic_ptr{"Image"_key, {}};
  proto->addField(
      "src"_key,
      field{
          std::string{""},
          attr<DisplayName>("Source"),
          attr<Description>("Resource file name in the session folder (upload via file service)."),
          attr<Category>("Content"),
          attr<Required>()});
  proto->addField(
      "width"_key,
      field{
          int32_t{0},
          attr<DisplayName>("Width"),
          attr<Description>("Display width in pixels; 0 uses the image's natural width."),
          attr<Category>("Layout"),
          attr<Range>(0, 16384),
          attr<Step>(1)});
  proto->addField(
      "height"_key,
      field{
          int32_t{0},
          attr<DisplayName>("Height"),
          attr<Description>("Display height in pixels; 0 uses the image's natural height."),
          attr<Category>("Layout"),
          attr<Range>(0, 16384),
          attr<Step>(1)});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Image"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>("Displays an image uploaded via the file service."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Image"_key));
}

} // namespace bdg::wish
