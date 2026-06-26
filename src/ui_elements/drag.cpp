// MIT License © 2025 Binary Dice Games
/// @file drag.cpp
/// @brief Registers DragFloat and DragInt prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_drag() {
  // DragFloat — click-and-drag (or double-click to type) a float value.
  {
    auto proto = dynamic_ptr{"DragFloat"_key, {}};
    proto->addField("label"_key, field{std::string{},
      attr<DisplayName>("Label"),
      attr<Description>("Caption shown to the left of the drag widget."),
      attr<Category>("Content")});
    proto->addField("value"_key, field{float{0.0f},
      attr<DisplayName>("Value"),
      attr<Description>("Current float value."),
      attr<Category>("State")});
    proto->addField("speed"_key, field{float{1.0f},
      attr<DisplayName>("Speed"),
      attr<Description>("Change per pixel dragged."),
      attr<Category>("Behavior")});
    proto->addField("min"_key, field{float{0.0f},
      attr<DisplayName>("Min"),
      attr<Description>("Lower clamp. When min == max the value is unclamped."),
      attr<Category>("Behavior")});
    proto->addField("max"_key, field{float{0.0f},
      attr<DisplayName>("Max"),
      attr<Description>("Upper clamp. When min == max the value is unclamped."),
      attr<Category>("Behavior")});
    proto->addField("format"_key, field{std::string{"%.3f"},
      attr<DisplayName>("Format"),
      attr<Description>("printf-style display format."),
      attr<Category>("Display")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DragFloat"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "A float value editor: click-and-drag to change, double-click to type. "
        "Emits 'changed' with {value: float} when edited."));
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key,
        dynamic::make_factory<ui_element>("wish"_key, "DragFloat"_key));
  }

  // DragInt — click-and-drag (or double-click to type) an integer value.
  {
    auto proto = dynamic_ptr{"DragInt"_key, {}};
    proto->addField("label"_key, field{std::string{},
      attr<DisplayName>("Label"),
      attr<Description>("Caption shown to the left of the drag widget."),
      attr<Category>("Content")});
    proto->addField("value"_key, field{int32_t{0},
      attr<DisplayName>("Value"),
      attr<Description>("Current integer value."),
      attr<Category>("State")});
    proto->addField("speed"_key, field{float{1.0f},
      attr<DisplayName>("Speed"),
      attr<Description>("Change per pixel dragged."),
      attr<Category>("Behavior")});
    proto->addField("min"_key, field{int32_t{0},
      attr<DisplayName>("Min"),
      attr<Description>("Lower clamp. When min == max the value is unclamped."),
      attr<Category>("Behavior")});
    proto->addField("max"_key, field{int32_t{0},
      attr<DisplayName>("Max"),
      attr<Description>("Upper clamp. When min == max the value is unclamped."),
      attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DragInt"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "An integer value editor: click-and-drag to change, double-click to type. "
        "Emits 'changed' with {value: int} when edited."));
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key,
        dynamic::make_factory<ui_element>("wish"_key, "DragInt"_key));
  }
}

}  // namespace bdg::wish
