// MIT License © 2025 Binary Dice Games
/// @file selectable.cpp
/// @brief Registers the Selectable prototype.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_selectable() {
  auto proto = dynamic_ptr{"Selectable"_rkey, {}};
  proto->addField(
      "label"_rkey,
      field{
          std::string{},
          attr<DisplayName>("Label"),
          attr<Description>("Text shown inside the selectable area."),
          attr<Category>("Content")});
  proto->addField(
      "selected"_rkey,
      field{
          false,
          attr<DisplayName>("Selected"),
          attr<Description>("Whether the item appears highlighted/selected."),
          attr<Category>("State")});
  proto->addField(
      "width"_rkey,
      field{
          float{0.0f},
          attr<DisplayName>("Width"),
          attr<Description>("Width in pixels. 0 fills the available width."),
          attr<Category>("Layout")});
  proto->addField(
      "height"_rkey,
      field{
          float{0.0f},
          attr<DisplayName>("Height"),
          attr<Description>("Height in pixels. 0 uses the default line height."),
          attr<Category>("Layout")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Selectable"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("A highlight-on-hover item suitable for building list boxes or menus. "
                        "Emits 'changed' with {selected: bool} on click."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Selectable"_key));
}

} // namespace bdg::wish
