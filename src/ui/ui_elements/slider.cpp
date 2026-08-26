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
    auto proto = dynamic_ptr{"SliderFloat"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{""},
            attr<DisplayName>("Label"),
            attr<Description>("Slider label text."),
            attr<Category>("Content")});
    proto->addField(
        "value"_rkey,
        field{
            0.0f,
            attr<DisplayName>("Value"),
            attr<Description>("Current float value (clamped to [min, max])."),
            attr<Category>("State"),
            attr<Step>(0.01)});
    proto->addField(
        "min"_rkey,
        field{
            0.0f,
            attr<DisplayName>("Min"),
            attr<Description>("Minimum selectable value."),
            attr<Category>("Behavior"),
            attr<Step>(0.01)});
    proto->addField(
        "max"_rkey,
        field{
            1.0f,
            attr<DisplayName>("Max"),
            attr<Description>("Maximum selectable value."),
            attr<Category>("Behavior"),
            attr<Step>(0.01)});
    proto->addField(
        "format"_rkey,
        field{
            std::string{"%.2f"},
            attr<DisplayName>("Format"),
            attr<Description>("printf format string for the displayed value."),
            attr<Category>("Appearance")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Slider (Float)"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>("A float-valued slider control."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_slider_float>("wish"_key, "SliderFloat"_key));
  }

  // SliderInt
  {
    auto proto = dynamic_ptr{"SliderInt"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{""},
            attr<DisplayName>("Label"),
            attr<Description>("Slider label text."),
            attr<Category>("Content")});
    proto->addField(
        "value"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Value"),
            attr<Description>("Current integer value (clamped to [min, max])."),
            attr<Category>("State"),
            attr<Step>(1)});
    proto->addField(
        "min"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Min"),
            attr<Description>("Minimum selectable value."),
            attr<Category>("Behavior"),
            attr<Step>(1)});
    proto->addField(
        "max"_rkey,
        field{
            int32_t{100},
            attr<DisplayName>("Max"),
            attr<Description>("Maximum selectable value."),
            attr<Category>("Behavior"),
            attr<Step>(1)});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Slider (Int)"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>("An integer-valued slider control."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_slider_int>("wish"_key, "SliderInt"_key));
  }
}

} // namespace bdg::wish
