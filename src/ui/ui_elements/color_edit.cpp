// MIT License © 2025 Binary Dice Games
/// @file color_edit.cpp
/// @brief Registers the ColorEdit prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_color_edit() {
  auto proto = dynamic_ptr{"ColorEdit"_rkey, {}};
  proto->addField(
      "label"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Label"),
          attr<Description>("Color picker label."),
          attr<Category>("Content")});
  proto->addField(
      "value"_rkey,
      field{
          std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f},
          attr<DisplayName>("Value"),
          attr<Description>("Current color as [r, g, b] or [r, g, b, a] components "
                            "in [0, 1]. The component count (3 or 4) selects "
                            "ColorEdit3 vs. ColorEdit4.")});
  proto->addField(
      "flags"_rkey,
      field{
          int32_t{0},
          attr<DisplayName>("Flags"),
          attr<Description>("ImGuiColorEditFlags bitmask (e.g. NoAlpha=2, "
                            "NoInputs=32, PickerHueWheel=16777216)."),
          attr<Category>("Behavior")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Color Edit"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("A color swatch that opens a picker popup when clicked. "
                        "Emits 'changed' with {value: [float, ...]} when the "
                        "color changes."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "ColorEdit"_key));
}

} // namespace bdg::wish
