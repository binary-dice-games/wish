// MIT License © 2025 Binary Dice Games
/// @file checkbox.cpp
/// @brief Registers the Checkbox prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_checkbox() {
  auto proto = dynamic_ptr{"Checkbox"_key, {}};
  proto->addField("label"_key, field{std::string{""}});
  proto->addField("value"_key, field{false});
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key);
}

}  // namespace bdg::wish
