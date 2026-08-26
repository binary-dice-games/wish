// MIT License © 2025 Binary Dice Games
/// @file label.cpp
/// @brief Registers the Label prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_label() {
  auto proto = dynamic_ptr{"Label"_rkey, {}};
  proto->addField(
      "text"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Text"),
          attr<Description>("Displayed text content."),
          attr<Category>("Content")});
  proto->addField(
      "text_color"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Text Color"),
          attr<Description>("Optional \"#RRGGBBAA\"/\"#RRGGBB\" text color override; empty uses the "
                            "current theme's text color."),
          attr<Category>("Appearance")});
  proto->addField(
      "text_color_light"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Text Color (Light Theme)"),
          attr<Description>("Optional \"#RRGGBBAA\"/\"#RRGGBB\" text color used instead of text_color "
                            "while the session's active theme is light-based (see "
                            "style_service::is_light_theme()); re-evaluated every frame, so it follows "
                            "a theme change made after this Label was created. Ignored when empty."),
          attr<Category>("Appearance")});
  proto->addField(
      "text_color_dark"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Text Color (Dark Theme)"),
          attr<Description>("Same as text_color_light, used instead while the active theme is "
                            "dark-based. Ignored when empty."),
          attr<Category>("Appearance")});
  proto->addField(
      "wrap"_rkey,
      field{
          bool{false},
          attr<DisplayName>("Wrap"),
          attr<Description>("When true, wraps text to the available width instead of overflowing/"
                            "clipping on one line."),
          attr<Category>("Appearance")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Label"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>("A read-only text element."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_label>("wish"_key, "Label"_key));
}

} // namespace bdg::wish
