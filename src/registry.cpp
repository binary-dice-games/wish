// MIT License © 2025 Binary Dice Games
/// @file registry.cpp
#include <wish/registry.hpp>
#include <wish/file_service.hpp>

#include "template_handler.hpp"
#include "ui_elements.hpp"

namespace bdg::wish {

void register_all() {
  // UI element classes — order matters: parents before children.
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
  // Protocol handler classes.
  register_template_handler();
  register_file_service();
}

}  // namespace bdg::wish
