// MIT License © 2025 Binary Dice Games
/// @file radio_button.cpp
/// @brief Registers the RadioButton prototype.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_radio_button() {
  auto proto = dynamic_ptr{"RadioButton"_key, {}};
  proto->addField("label"_key, field{std::string{},
    attr<DisplayName>("Label"),
    attr<Description>("Caption shown beside the radio circle."),
    attr<Category>("Content")});
  proto->addField("active"_key, field{false,
    attr<DisplayName>("Active"),
    attr<Description>(
        "Whether this radio button is in the selected (filled) state. "
        "The server is responsible for maintaining mutual exclusivity within a group."),
    attr<Category>("State")});
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key, {
    attr<DisplayName>("RadioButton"),
    attr<Description>(
        "A single radio button. "
        "Emits 'clicked' when the user presses the button.")});
}

}  // namespace bdg::wish
