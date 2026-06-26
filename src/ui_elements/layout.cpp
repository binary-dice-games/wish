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
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Layout"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "Base container that arranges child elements."));
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key,
        dynamic::make_factory<ui_element>("wish"_key, "Layout"_key));
  }

  // VerticalLayout — stacks children top-to-bottom; no extra fields.
  {
    auto proto = dynamic_ptr{"VerticalLayout"_key, {}};
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Vertical Layout"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>("Stacks children top-to-bottom."));
    dynamic::addClass("wish"_key, std::move(proto), "Layout"_key,
        dynamic::make_factory<ui_element>("wish"_key, "VerticalLayout"_key));
  }

  // HorizontalLayout — places children side by side; no extra fields.
  {
    auto proto = dynamic_ptr{"HorizontalLayout"_key, {}};
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Horizontal Layout"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>("Arranges children left-to-right."));
    dynamic::addClass("wish"_key, std::move(proto), "Layout"_key,
        dynamic::make_factory<ui_element>("wish"_key, "HorizontalLayout"_key));
  }
}

}  // namespace bdg::wish
