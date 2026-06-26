// MIT License © 2025 Binary Dice Games
/// @file progress_bar.cpp
/// @brief Registers the ProgressBar prototype.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_progress_bar() {
  auto proto = dynamic_ptr{"ProgressBar"_key, {}};
  proto->addField("value"_key, field{float{0.0f},
    attr<DisplayName>("Value"),
    attr<Description>("Fill fraction: 0.0 (empty) to 1.0 (full)."),
    attr<Category>("State")});
  proto->addField("label"_key, field{std::string{},
    attr<DisplayName>("Label"),
    attr<Description>("Optional text drawn on top of the bar."),
    attr<Category>("Content")});
  proto->addField("width"_key, field{float{-1.0f},
    attr<DisplayName>("Width"),
    attr<Description>("Bar width in pixels. -1 fills the available width."),
    attr<Category>("Layout")});
  proto->addField("height"_key, field{float{0.0f},
    attr<DisplayName>("Height"),
    attr<Description>("Bar height in pixels. 0 uses the default height."),
    attr<Category>("Layout")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("ProgressBar"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "A non-interactive horizontal progress bar. No events."));
  dynamic::addClass("wish"_key, std::move(proto), "Element"_key,
      dynamic::make_factory<ui_element>("wish"_key, "ProgressBar"_key));
}

}  // namespace bdg::wish
