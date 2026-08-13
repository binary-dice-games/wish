// MIT License © 2025 Binary Dice Games
/// @file console_renderer.cpp
/// @brief Implementation of console_renderer.
#include <server/console_renderer.hpp>

#include "src/bison/bison_common.hpp"

#include <iostream>
#include <optional>
#include <string>

namespace bdg::wish {

using namespace bison; // NOLINT -- brings in operator""_key, matching client.cpp's convention.

void console_renderer::render_node(const ui_element& node, const context& s) {
  std::string indent(static_cast<std::size_t>(depth_) * 2, ' ');

  bison::key_t klass = node.get_as<bison::key_t>(dynamic::CLASS, bison::key_t{});
  std::optional<std::string> class_name = lookup_registered_key_name(klass);

  std::cout << indent << "- " << (class_name ? *class_name : "?");

  if (const auto* name_field = node.findField("name"_key))
    if (name_field->is<std::string>())
      std::cout << " name=\"" << name_field->as<std::string>() << "\"";

  std::cout << "\n";

  ++depth_;
  render_children(*this, node, s);
  --depth_;
}

} // namespace bdg::wish
