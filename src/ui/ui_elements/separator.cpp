// MIT License © 2025 Binary Dice Games
/// @file separator.cpp
/// @brief Registers the Separator prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_separator() {
  {
    auto proto = dynamic_ptr{"Separator"_rkey, {}};
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Separator"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>("A visual horizontal rule between elements."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Separator"_key));
  }
  {
    auto proto = dynamic_ptr{"SeparatorText"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Text shown inline with the separator line."),
            attr<Category>("Content")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("SeparatorText"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A horizontal rule with a text label embedded in the line."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Element"_key,
        dynamic::make_factory<ui_element>("wish"_key, "SeparatorText"_key));
  }
}

} // namespace bdg::wish
