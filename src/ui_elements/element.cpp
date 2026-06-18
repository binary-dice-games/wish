// MIT License © 2025 Binary Dice Games
/// @file element.cpp
/// @brief Registers the Element base prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_element() {
  auto proto = dynamic_ptr{"Element"_key, {}};
  proto->addField("visible"_key, field{true,
    attr<DisplayName>("Visible"),
    attr<Description>("Whether the element is rendered."),
    attr<Category>("Behavior")});
  proto->addField("children"_key, field{dynamic_ptr{key_t{0U}, {}},
    attr<DisplayName>("Children"),
    attr<Description>("Nested child elements."),
    attr<Category>("Layout")});
  dynamic::addClass("wish"_key, std::move(proto), key_t{0U}, {
    attr<DisplayName>("Element"),
    attr<Description>("Base class for all UI elements.")});
}

}  // namespace bdg::wish
