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
          attr<Description>("ImGuiColorEditFlags bitmask (combine names with '|')."),
          attr<Category>("Behavior"),
          attr<EnumFlags>(EnumFlags::table{
              {"NoAlpha", 1 << 1},
              {"NoPicker", 1 << 2},
              {"NoOptions", 1 << 3},
              {"NoSmallPreview", 1 << 4},
              {"NoInputs", 1 << 5},
              {"NoTooltip", 1 << 6},
              {"NoLabel", 1 << 7},
              {"NoSidePreview", 1 << 8},
              {"NoDragDrop", 1 << 9},
              {"NoBorder", 1 << 10},
              {"NoColorMarkers", 1 << 11},
              {"AlphaOpaque", 1 << 12},
              {"AlphaNoBg", 1 << 13},
              {"AlphaPreviewHalf", 1 << 14},
              {"AlphaBar", 1 << 18},
              {"HDR", 1 << 19},
              {"DisplayRGB", 1 << 20},
              {"DisplayHSV", 1 << 21},
              {"DisplayHex", 1 << 22},
              {"Uint8", 1 << 23},
              {"Float", 1 << 24},
              {"PickerHueBar", 1 << 25},
              {"PickerHueWheel", 1 << 26},
              {"PickerNoRotate", 1 << 27},
              {"InputRGB", 1 << 28},
              {"InputHSV", 1 << 29},
          })});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Color Edit"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("A color swatch that opens a picker popup when clicked. "
                        "Emits 'changed' with {value: [float, ...]} when the "
                        "color changes."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_color_edit>("wish"_key, "ColorEdit"_key));
}

} // namespace bdg::wish
