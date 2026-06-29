// MIT License © 2025 Binary Dice Games
/// @file renderer.cpp
/// @brief Implementation of render_children.
#include <renderer.hpp>

namespace bdg::wish {

void render_children(renderer& r, const ui_element& node, const session& s) {
  node.for_each_child_ordered([&](bison::key_t, ui_element& child) { r.render_node(child, s); });
}

} // namespace bdg::wish
