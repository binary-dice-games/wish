// MIT License © 2025 Binary Dice Games
/// @file registry.cpp
#include <wish/registry.hpp>

#include "import_handler.hpp"
#include "template_handler.hpp"
#include "ui_elements.hpp"

namespace bdg::wish {

void register_all() {
  // Order matters: parents must be registered before children.
  register_element();   // root base: visible, children
  register_layout();    // Layout < Element; VerticalLayout, HorizontalLayout < Layout
  register_window();
  register_label();
  register_button();
  register_checkbox();
  register_slider();    // SliderFloat and SliderInt
  register_input_text();
  register_image();
  register_separator();
  // Protocol handler classes used by wish::client helpers.
  import_handler::register_class();
  template_handler::register_class();
}

}  // namespace bdg::wish
