// MIT License © 2025 Binary Dice Games
/// @file tree.cpp
/// @brief Registers TreeNode and CollapsingHeader prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_tree() {
  // TreeNode — collapsible node with an arrow; children are sub-tree content.
  {
    auto proto = dynamic_ptr{"TreeNode"_key, {}};
    proto->addField("label"_key, field{std::string{},
      attr<DisplayName>("Label"),
      attr<Description>("Text shown next to the collapse arrow."),
      attr<Category>("Content")});
    proto->addField("open"_key, field{false,
      attr<DisplayName>("Open"),
      attr<Description>(
          "Initial open/closed state (applied once via ImGuiCond_Once; "
          "ImGui manages the state afterward)."),
      attr<Category>("State")});
    proto->addField("leaf"_key, field{false,
      attr<DisplayName>("Leaf"),
      attr<Description>(
          "When true, renders as a leaf node without an arrow and does not show children."),
      attr<Category>("Behavior")});
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key, {
      attr<DisplayName>("TreeNode"),
      attr<Description>(
          "A collapsible tree node with an arrow. "
          "Emits 'toggled' with {open: bool} when expanded or collapsed.")});
  }

  // CollapsingHeader — bold section header that collapses its children.
  {
    auto proto = dynamic_ptr{"CollapsingHeader"_key, {}};
    proto->addField("label"_key, field{std::string{},
      attr<DisplayName>("Label"),
      attr<Description>("Header text."),
      attr<Category>("Content")});
    dynamic::addClass("wish"_key, std::move(proto), "Element"_key, {
      attr<DisplayName>("CollapsingHeader"),
      attr<Description>(
          "A bold section header that toggles visibility of its children. "
          "Emits 'toggled' with {open: bool} when expanded or collapsed.")});
  }
}

}  // namespace bdg::wish
