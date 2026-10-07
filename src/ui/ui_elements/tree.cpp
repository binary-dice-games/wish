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
    auto proto = dynamic_ptr{"TreeNode"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Text shown next to the collapse arrow."),
            attr<Category>("Content")});
    proto->addField(
        "open"_rkey,
        field{
            false,
            attr<DisplayName>("Open"),
            attr<Description>("Open/closed state. The initial value is applied once and ImGui "
                              "manages the state afterward; the renderer writes a user toggle back "
                              "into this field, and a later change to it expands/collapses the node."),
            attr<Category>("State")});
    proto->addField(
        "leaf"_rkey,
        field{
            false,
            attr<DisplayName>("Leaf"),
            attr<Description>("When true, renders as a leaf node without an arrow and does not show children."),
            attr<Category>("Behavior")});
    proto->addField(
        "selected"_rkey,
        field{
            false,
            attr<DisplayName>("Selected"),
            attr<Description>("Draws the node highlighted. Application-owned: set it from a 'clicked' "
                              "handler. A node that becomes selected is scrolled into view."),
            attr<Category>("State")});
    proto->addField(
        "open_on_arrow"_rkey,
        field{
            false,
            attr<DisplayName>("Open On Arrow"),
            attr<Description>("When true, only the arrow (or a double click) expands/collapses the node, "
                              "so a click on the label just emits 'clicked'; the node also spans the "
                              "available width."),
            attr<Category>("Behavior")});
    proto->addField(
        "icon"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Icon"),
            attr<Description>("Image drawn between the arrow and the label, at text height and tinted "
                              "to the text color (e.g. \"res/icons/folder.png\"). Same security "
                              "contract as Image.src: a path relative to the session resource "
                              "directory; absolute paths are rejected unless the server has enabled "
                              "allow_absolute_paths, and a path escaping the sandbox draws no icon."),
            attr<Category>("Content")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("TreeNode"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A collapsible tree node with an arrow. "
                          "Emits 'toggled' with {open: bool} when the user expands or collapses it, and "
                          "'clicked' when it is clicked without toggling."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_tree_node>("wish"_key, "TreeNode"_key));
  }

  // CollapsingHeader — bold section header that collapses its children.
  {
    auto proto = dynamic_ptr{"CollapsingHeader"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{std::string{}, attr<DisplayName>("Label"), attr<Description>("Header text."), attr<Category>("Content")});
    proto->addField(
        "open"_rkey,
        field{
            true,
            attr<DisplayName>("Open"),
            attr<Description>("Expanded/collapsed state. Unlike TreeNode's 'open', this is forced "
                              "into ImGui every frame (ImGuiCond_Always): the server owns the state, "
                              "ImGui never drives it. Update this field in the 'toggled' handler to "
                              "keep the header responsive to clicks."),
            attr<Category>("State")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("CollapsingHeader"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A bold section header that toggles visibility of its children. "
                          "Emits 'toggled' with {open: bool} when expanded or collapsed. "
                          "Its 'open' field is server-authoritative (forced every frame)."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Element"_key,
        dynamic::make_factory<ui_collapsing_header>("wish"_key, "CollapsingHeader"_key));
  }
}

} // namespace bdg::wish
