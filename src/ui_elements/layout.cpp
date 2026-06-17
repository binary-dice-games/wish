// MIT License © 2025 Binary Dice Games
/// @file layout.cpp
/// @brief Registers Layout, VerticalLayout, and HorizontalLayout prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_layout() {
  // Layout — intermediate base that adds a spacing field.
  {
    auto proto = dynamic_ptr{"Layout"_key, {}};
    proto->addField("spacing"_key, field{0.0f});
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key);
  }

  // VerticalLayout — stacks children top-to-bottom; no extra fields.
  {
    auto proto = dynamic_ptr{"VerticalLayout"_key, {}};
    dynamic::addClass("wish"_key, std::move(proto), "Layout"_key);
  }

  // HorizontalLayout — places children side by side; no extra fields.
  {
    auto proto = dynamic_ptr{"HorizontalLayout"_key, {}};
    dynamic::addClass("wish"_key, std::move(proto), "Layout"_key);
  }
}

}  // namespace bdg::wish
