// MIT License © 2025 Binary Dice Games
/// @file window.cpp
/// @brief Registers the Window prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

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
    attr<Description>("ImGui window flags bitmask."),
    attr<Category>("Behavior")});
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key, {
    attr<DisplayName>("Window"),
    attr<Description>("A top-level window container.")});
}

}  // namespace bdg::wish
