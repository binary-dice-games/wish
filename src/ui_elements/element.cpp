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
  proto->addField("order"_key, field{int32_t{0},
    attr<DisplayName>("Order"),
    attr<Description>("Render order within parent children. Lower values render first."),
    attr<Category>("Layout")});
  proto->addField("font_path"_key, field{std::string{},
    attr<DisplayName>("Font Path"),
    attr<Description>("Path to a TTF font file. Relative paths are sandboxed "
                      "to the session resource directory; absolute paths "
                      "require server::set_allow_absolute_paths(true)."),
    attr<Category>("Appearance")});
  proto->addField("font_size"_key, field{float{0.0f},
    attr<DisplayName>("Font Size"),
    attr<Description>("Font size in pixels. 0 uses the default ImGui font."),
    attr<Category>("Appearance")});
  dynamic::addClass("wish"_key, std::move(proto), key_t{0U}, {
    attr<DisplayName>("Element"),
    attr<Description>("Base class for all UI elements.")});
}

}  // namespace bdg::wish
