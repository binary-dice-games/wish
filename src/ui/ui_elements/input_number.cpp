// MIT License © 2025 Binary Dice Games
/// @file input_number.cpp
/// @brief Registers InputInt and InputFloat prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_input_number() {
  // InputInt — integer input with +/- step buttons.
  {
    auto proto = dynamic_ptr{"InputInt"_key, {}};
    proto->addField(
        "label"_key,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Caption shown to the left of the input box."),
            attr<Category>("Content")});
    proto->addField(
        "value"_key,
        field{
            int32_t{0},
            attr<DisplayName>("Value"),
            attr<Description>("Current integer value."),
            attr<Category>("State")});
    proto->addField(
        "step"_key,
        field{
            int32_t{1},
            attr<DisplayName>("Step"),
            attr<Description>("Amount added/subtracted by the +/- buttons."),
            attr<Category>("Behavior")});
    proto->addField(
        "step_fast"_key,
        field{
            int32_t{100},
            attr<DisplayName>("Step Fast"),
            attr<Description>("Amount added/subtracted when Ctrl is held while clicking +/-."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("InputInt"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("An integer input field with increment/decrement buttons. "
                          "Emits 'changed' with {value: int} when edited."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "InputInt"_key));
  }

  // InputFloat — float input with optional step buttons.
  {
    auto proto = dynamic_ptr{"InputFloat"_key, {}};
    proto->addField(
        "label"_key,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Caption shown to the left of the input box."),
            attr<Category>("Content")});
    proto->addField(
        "value"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Value"),
            attr<Description>("Current float value."),
            attr<Category>("State")});
    proto->addField(
        "step"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Step"),
            attr<Description>("Amount added/subtracted by the +/- buttons. 0 hides the buttons."),
            attr<Category>("Behavior")});
    proto->addField(
        "step_fast"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Step Fast"),
            attr<Description>("Amount added/subtracted when Ctrl is held. 0 falls back to step."),
            attr<Category>("Behavior")});
    proto->addField(
        "format"_key,
        field{
            std::string{"%.3f"},
            attr<DisplayName>("Format"),
            attr<Description>("printf-style display format, e.g. \"%.2f\"."),
            attr<Category>("Display")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("InputFloat"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A float input field with optional increment/decrement buttons. "
                          "Emits 'changed' with {value: float} when edited."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "InputFloat"_key));
  }
}

} // namespace bdg::wish
