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
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Button"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>("A clickable button that emits a clicked event."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Button"_key));
}

} // namespace bdg::wish
