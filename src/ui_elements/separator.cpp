// MIT License © 2025 Binary Dice Games
/// @file separator.cpp
/// @brief Registers the Separator prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_separator() {
  auto proto = dynamic_ptr{"Separator"_key, {}};
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key);
}

}  // namespace bdg::wish
