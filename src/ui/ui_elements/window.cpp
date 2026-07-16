// MIT License © 2025 Binary Dice Games
/// @file window.cpp
/// @brief Registers the Window prototype in the "wish" bison namespace.
#include "ui/ui_elements/window.hpp"

#include "src/bison/bison_object.hpp"

#include "ui/ui_elements/ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

window::window(bison::dynamic&& base) : ui_root(std::move(base)) {}

void register_window() {
  auto proto = dynamic_ptr{"Window"_key, {}};
  proto->addField(
      "title"_key,
      field{
          std::string{""},
          attr<DisplayName>("Title"),
          attr<Description>("Window title bar text."),
          attr<Category>("Appearance")});
  proto->addField(
      "width"_key,
      field{
          int32_t{0},
          attr<DisplayName>("Width"),
          attr<Description>("Window width in pixels."),
          attr<Category>("Layout"),
          attr<Range>(0, 16384),
          attr<Step>(1)});
  proto->addField(
      "height"_key,
      field{
          int32_t{0},
          attr<DisplayName>("Height"),
          attr<Description>("Window height in pixels."),
          attr<Category>("Layout"),
          attr<Range>(0, 16384),
          attr<Step>(1)});
  proto->addField(
      "pos_x"_key,
      field{
          int32_t{0},
          attr<DisplayName>("X"),
          attr<Description>("Horizontal position in pixels."),
          attr<Category>("Layout"),
          attr<Step>(1)});
  proto->addField(
      "pos_y"_key,
      field{
          int32_t{0},
          attr<DisplayName>("Y"),
          attr<Description>("Vertical position in pixels."),
          attr<Category>("Layout"),
          attr<Step>(1)});
  proto->addField(
      "closable"_key,
      field{
          bool{false},
          attr<DisplayName>("Closable"),
          attr<Description>("Show a close button (X) on the window title bar. "
                            "Clicking it emits the 'closed' event."),
          attr<Category>("Behavior")});
  proto->addField(
      "modal"_key,
      field{
          bool{false},
          attr<DisplayName>("Modal"),
          attr<Description>("Render as a true input-blocking modal popup (ImGui "
                            "BeginPopupModal) instead of a normal floating/dockable "
                            "window. Forces NoDocking. Combine with closable to show "
                            "a title-bar X that also emits 'closed'."),
          attr<Category>("Behavior")});
  proto->addField(
      "flags"_key,
      field{
          int32_t{0},
          attr<DisplayName>("Flags"),
          attr<Description>("Window flags bitmask (combine names with '|')."),
          attr<Category>("Behavior"),
          attr<EnumFlags>(EnumFlags::table{
              {"NoTitleBar", 1 << 0},
              {"NoResize", 1 << 1},
              {"NoMove", 1 << 2},
              {"NoScrollbar", 1 << 3},
              {"NoScrollWithMouse", 1 << 4},
              {"NoCollapse", 1 << 5},
              {"AlwaysAutoResize", 1 << 6},
              {"NoBackground", 1 << 7},
              {"NoSavedSettings", 1 << 8},
              {"NoMouseInputs", 1 << 9},
              {"MenuBar", 1 << 10},
              {"HorizontalScrollbar", 1 << 11},
              {"NoFocusOnAppearing", 1 << 12},
              {"NoBringToFrontOnFocus", 1 << 13},
              {"AlwaysVerticalScrollbar", 1 << 14},
              {"AlwaysHorizontalScrollbar", 1 << 15},
              {"NoNavInputs", 1 << 16},
              {"NoNavFocus", 1 << 17},
              {"UnsavedDocument", 1 << 18},
              {"NoDocking", 1 << 19},
              // Convenience composites — listed after single-bit flags so format()
              // prefers the fine-grained names when decomposing a value.
              {"NoNav", (1 << 16) | (1 << 17)},
              {"NoDecoration", (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5)},
              {"NoInputs", (1 << 9) | (1 << 16) | (1 << 17)},
          })});
  // Attach class-level attrs manually (no combined attrs+factory overload exists).
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Window"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>("A top-level window container."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<window>("wish"_key, "Window"_key));
}

} // namespace bdg::wish
