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
    proto->addField("spacing"_key, field{0.0f,
      attr<DisplayName>("Spacing"),
      attr<Description>("Space between child elements in pixels."),
      attr<Category>("Layout"),
      attr<Range>(0, 256),
      attr<Step>(0.5)});
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key, {
      attr<DisplayName>("Layout"),
      attr<Description>("Base container that arranges child elements.")});
  }

  // VerticalLayout — stacks children top-to-bottom; no extra fields.
  {
    auto proto = dynamic_ptr{"VerticalLayout"_key, {}};
    dynamic::addClass("wish"_key, std::move(proto), "Layout"_key, {
      attr<DisplayName>("Vertical Layout"),
      attr<Description>("Stacks children top-to-bottom.")});
  }

  // HorizontalLayout — places children side by side; no extra fields.
  {
    auto proto = dynamic_ptr{"HorizontalLayout"_key, {}};
    dynamic::addClass("wish"_key, std::move(proto), "Layout"_key, {
      attr<DisplayName>("Horizontal Layout"),
      attr<Description>("Arranges children left-to-right.")});
  }
}

}  // namespace bdg::wish
