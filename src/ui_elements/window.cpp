// MIT License © 2025 Binary Dice Games
/// @file window.cpp
/// @brief Registers the Window prototype in the "wish" bison namespace.
#include <wish/window.hpp>

#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

window::window(bison::dynamic&& base) : ui_root(std::move(base)) {}

void register_window() {
  auto proto = dynamic_ptr{"Window"_key, {}};
  proto->addField("title"_key, field{std::string{""},
    attr<DisplayName>("Title"),
    attr<Description>("Window title bar text."),
    attr<Category>("Appearance")});
  proto->addField("width"_key, field{int32_t{0},
    attr<DisplayName>("Width"),
    attr<Description>("Window width in pixels."),
    attr<Category>("Layout"),
    attr<Range>(0, 16384),
    attr<Step>(1)});
  proto->addField("height"_key, field{int32_t{0},
    attr<DisplayName>("Height"),
    attr<Description>("Window height in pixels."),
    attr<Category>("Layout"),
    attr<Range>(0, 16384),
    attr<Step>(1)});
  proto->addField("pos_x"_key, field{int32_t{0},
    attr<DisplayName>("X"),
    attr<Description>("Horizontal position in pixels."),
    attr<Category>("Layout"),
    attr<Step>(1)});
  proto->addField("pos_y"_key, field{int32_t{0},
    attr<DisplayName>("Y"),
    attr<Description>("Vertical position in pixels."),
    attr<Category>("Layout"),
    attr<Step>(1)});
  proto->addField("flags"_key, field{int32_t{0},
    attr<DisplayName>("Flags"),
    attr<Description>("ImGui window flags bitmask (combine names with '|')."),
    attr<Category>("Behavior"),
    attr<EnumFlags>(EnumFlags::table{
      {"ImGuiWindowFlags_NoTitleBar",                1 << 0},
      {"ImGuiWindowFlags_NoResize",                  1 << 1},
      {"ImGuiWindowFlags_NoMove",                    1 << 2},
      {"ImGuiWindowFlags_NoScrollbar",               1 << 3},
      {"ImGuiWindowFlags_NoScrollWithMouse",         1 << 4},
      {"ImGuiWindowFlags_NoCollapse",                1 << 5},
      {"ImGuiWindowFlags_AlwaysAutoResize",          1 << 6},
      {"ImGuiWindowFlags_NoBackground",              1 << 7},
      {"ImGuiWindowFlags_NoSavedSettings",           1 << 8},
      {"ImGuiWindowFlags_NoMouseInputs",             1 << 9},
      {"ImGuiWindowFlags_MenuBar",                   1 << 10},
      {"ImGuiWindowFlags_HorizontalScrollbar",       1 << 11},
      {"ImGuiWindowFlags_NoFocusOnAppearing",        1 << 12},
      {"ImGuiWindowFlags_NoBringToFrontOnFocus",     1 << 13},
      {"ImGuiWindowFlags_AlwaysVerticalScrollbar",   1 << 14},
      {"ImGuiWindowFlags_AlwaysHorizontalScrollbar", 1 << 15},
      {"ImGuiWindowFlags_NoNavInputs",               1 << 16},
      {"ImGuiWindowFlags_NoNavFocus",                1 << 17},
      {"ImGuiWindowFlags_UnsavedDocument",           1 << 18},
      {"ImGuiWindowFlags_NoDocking",                 1 << 19},
      // Convenience composites — listed after single-bit flags so format()
      // prefers the fine-grained names when decomposing a value.
      {"ImGuiWindowFlags_NoNav",        (1 << 16) | (1 << 17)},
      {"ImGuiWindowFlags_NoDecoration", (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5)},
      {"ImGuiWindowFlags_NoInputs",     (1 << 9) | (1 << 16) | (1 << 17)},
    })});
  // Attach class-level attrs manually (no combined attrs+factory overload exists).
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Window"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>("A top-level window container."));
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key,
      dynamic::make_factory<window>("wish"_key, "Window"_key));
}

}  // namespace bdg::wish
