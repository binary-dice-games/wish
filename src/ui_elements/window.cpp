// MIT License © 2025 Binary Dice Games
/// @file window.cpp
/// @brief Registers the Window prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_window() {
  auto proto = dynamic_ptr{"Window"_key, {}};
  proto->addField("title"_key,  field{std::string{""}});
  proto->addField("width"_key,  field{int32_t{0}});
  proto->addField("height"_key, field{int32_t{0}});
  proto->addField("pos_x"_key,  field{int32_t{0}});
  proto->addField("pos_y"_key,  field{int32_t{0}});
  proto->addField("flags"_key,  field{int32_t{0}});
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key);
}

}  // namespace bdg::wish
