// MIT License © 2025 Binary Dice Games
/// @file input_text.cpp
/// @brief Registers the InputText prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_input_text() {
  auto proto = dynamic_ptr{"InputText"_key, {}};
  proto->addField("label"_key,      field{std::string{""}});
  proto->addField("value"_key,      field{std::string{""}});
  proto->addField("hint"_key,       field{std::string{""}});
  proto->addField("max_length"_key, field{int32_t{256}});
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key);
}

}  // namespace bdg::wish
