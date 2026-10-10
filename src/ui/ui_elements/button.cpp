// MIT License © 2025 Binary Dice Games
/// @file button.cpp
/// @brief Registers the Button prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_button() {
  auto proto = dynamic_ptr{"Button"_rkey, {}};
  proto->addField(
      "label"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Label"),
          attr<Description>("Button caption text."),
          attr<Category>("Content")});
  proto->addField(
      "icon"_rkey,
      field{
          std::string{},
          attr<DisplayName>("Icon"),
          attr<Description>("Image drawn left of the label, at text height and tinted to the text "
                            "color (e.g. \"res/icons/settings.png\"). With an empty label the "
                            "button is a square icon-only button -- give it a 'tooltip'. "
                            "Same security contract as Image.src: a path relative to the session "
                            "resource directory; absolute paths are rejected unless the server has "
                            "enabled allow_absolute_paths, and a path escaping the sandbox draws no "
                            "icon."),
          attr<Category>("Content")});
  proto->addField(
      "icon_color"_rkey,
      field{
          std::string{},
          attr<DisplayName>("Icon Color"),
          attr<Description>("Optional \"#RRGGBBAA\"/\"#RRGGBB\" tint for 'icon'; empty uses the "
                            "current theme's text color."),
          attr<Category>("Appearance")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Button"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>("A clickable button that emits a clicked event."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_button>("wish"_key, "Button"_key));
}

} // namespace bdg::wish
