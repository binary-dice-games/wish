// MIT License © 2025 Binary Dice Games
/// @file input_text.cpp
/// @brief Registers the InputText prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_input_text() {
  auto proto = dynamic_ptr{"InputText"_key, {}};
  proto->addField("label"_key, field{std::string{""},
    attr<DisplayName>("Label"),
    attr<Description>("Input field label."),
    attr<Category>("Content")});
  proto->addField("value"_key, field{std::string{""},
    attr<DisplayName>("Value"),
    attr<Description>("Current text value."),
    attr<Category>("State")});
  proto->addField("hint"_key, field{std::string{""},
    attr<DisplayName>("Hint"),
    attr<Description>("Placeholder text shown when the field is empty."),
    attr<Category>("Content")});
  proto->addField("max_length"_key, field{int32_t{256},
    attr<DisplayName>("Max Length"),
    attr<Description>("Maximum number of characters the user may enter."),
    attr<Category>("Behavior"),
    attr<Range>(1, 65536),
    attr<Step>(1)});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Input Text"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>("A single-line text input field."));
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key,
      dynamic::make_factory<ui_element>("wish"_key, "InputText"_key));
}

}  // namespace bdg::wish
