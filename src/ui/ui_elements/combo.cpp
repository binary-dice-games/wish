// MIT License © 2025 Binary Dice Games
/// @file combo.cpp
/// @brief Registers the Combo prototype.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_combo() {
  auto proto = dynamic_ptr{"Combo"_key, {}};
  proto->addField(
      "label"_key,
      field{
          std::string{},
          attr<DisplayName>("Label"),
          attr<Description>("Caption shown to the left of the dropdown."),
          attr<Category>("Content")});
  proto->addField(
      "items"_key,
      field{
          std::string{},
          attr<DisplayName>("Items"),
          attr<Description>("Newline-separated list of option strings (e.g. \"A\\nB\\nC\")."),
          attr<Category>("Content")});
  proto->addField(
      "value"_key,
      field{
          int32_t{0},
          attr<DisplayName>("Value"),
          attr<Description>("Index of the currently selected item (0-based)."),
          attr<Category>("State")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Combo"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("A drop-down selection widget. "
                        "Emits 'changed' with {value: index, text: string} when the selection changes."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Combo"_key));
}

} // namespace bdg::wish
