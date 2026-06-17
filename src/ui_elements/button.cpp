// MIT License © 2025 Binary Dice Games
/// @file button.cpp
/// @brief Registers the Button prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_button() {
  auto proto = dynamic_ptr{"Button"_key, {}};
  proto->addField("label"_key, field{std::string{""}});
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key);
}

}  // namespace bdg::wish
