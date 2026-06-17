// MIT License © 2025 Binary Dice Games
/// @file slider.cpp
/// @brief Registers SliderFloat and SliderInt prototypes in the "wish" namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_slider() {
  // SliderFloat
  {
    auto proto = dynamic_ptr{"SliderFloat"_key, {}};
    proto->addField("label"_key,  field{std::string{""}});
    proto->addField("value"_key,  field{0.0f});
    proto->addField("min"_key,    field{0.0f});
    proto->addField("max"_key,    field{1.0f});
    proto->addField("format"_key, field{std::string{"%.2f"}});
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key);
  }

  // SliderInt
  {
    auto proto = dynamic_ptr{"SliderInt"_key, {}};
    proto->addField("label"_key, field{std::string{""}});
    proto->addField("value"_key, field{int32_t{0}});
    proto->addField("min"_key,   field{int32_t{0}});
    proto->addField("max"_key,   field{int32_t{100}});
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key);
  }
}

}  // namespace bdg::wish
