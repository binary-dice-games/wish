// MIT License © 2025 Binary Dice Games
/// @file button.cpp
/// @brief Registers the Button prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_button() {
  auto proto = dynamic_ptr{"Button"_key, {}};
  proto->addField("label"_key, field{std::string{""},
    attr<DisplayName>("Label"),
    attr<Description>("Button caption text."),
    attr<Category>("Content")});
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key, {
    attr<DisplayName>("Button"),
    attr<Description>(
        "A clickable button that emits a clicked event.")});
}

}  // namespace bdg::wish
