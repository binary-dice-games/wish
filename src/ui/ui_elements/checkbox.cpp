// MIT License © 2025 Binary Dice Games
/// @file checkbox.cpp
/// @brief Registers the Checkbox prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_checkbox() {
  auto proto = dynamic_ptr{"Checkbox"_rkey, {}};
  proto->addField(
      "label"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Label"),
          attr<Description>("Checkbox caption text."),
          attr<Category>("Content")});
  proto->addField(
      "value"_rkey,
      field{false, attr<DisplayName>("Value"), attr<Description>("Current checked state."), attr<Category>("State")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Checkbox"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>("A toggleable checkbox with a label."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_checkbox>("wish"_key, "Checkbox"_key));
}

} // namespace bdg::wish
