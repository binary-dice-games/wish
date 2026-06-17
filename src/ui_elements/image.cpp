// MIT License © 2025 Binary Dice Games
/// @file image.cpp
/// @brief Registers the Image prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_image() {
  auto proto = dynamic_ptr{"Image"_key, {}};
  proto->addField("src"_key,    field{std::string{""}});
  proto->addField("width"_key,  field{int32_t{0}});
  proto->addField("height"_key, field{int32_t{0}});
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key);
}

}  // namespace bdg::wish
