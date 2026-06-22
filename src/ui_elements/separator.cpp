// MIT License © 2025 Binary Dice Games
/// @file separator.cpp
/// @brief Registers the Separator prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_separator() {
  {
    auto proto = dynamic_ptr{"Separator"_key, {}};
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key, {
      attr<DisplayName>("Separator"),
      attr<Description>("A visual horizontal rule between elements.")});
  }
  {
    auto proto = dynamic_ptr{"SeparatorText"_key, {}};
    proto->addField("label"_key, field{std::string{},
      attr<DisplayName>("Label"),
      attr<Description>("Text shown inline with the separator line."),
      attr<Category>("Content")});
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key, {
      attr<DisplayName>("SeparatorText"),
      attr<Description>("A horizontal rule with a text label embedded in the line.")});
  }
}

}  // namespace bdg::wish
