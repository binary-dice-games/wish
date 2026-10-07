// MIT License © 2026 Binary Dice Games
/// @file treemap.cpp
/// @brief Registers the Treemap prototype.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_treemap() {
  auto proto = dynamic_ptr{"Treemap"_rkey, {}};
  proto->addField(
      "parents"_rkey,
      field{
          std::vector<int32_t>{},
          attr<DisplayName>("Parents"),
          attr<Description>("One entry per node: the index of its parent. Node 0 is the root (its entry "
                            "is ignored); every other node must come after its parent."),
          attr<Category>("Data")});
  proto->addField(
      "sizes"_rkey,
      field{
          std::vector<float>{},
          attr<DisplayName>("Sizes"),
          attr<Description>("Parallel array with parents: each node's weight. A node's children share its "
                            "rectangle in proportion to their sizes; a node with size 0 is not drawn."),
          attr<Category>("Data")});
  proto->addField(
      "colors"_rkey,
      field{
          std::vector<int32_t>{},
          attr<DisplayName>("Colors"),
          attr<Description>("Parallel array with parents: each node's packed 0xRRGGBBAA fill color. A "
                            "missing entry, or 0, picks a color from a built-in palette."),
          attr<Category>("Data")});
  proto->addField(
      "labels"_rkey,
      field{
          std::string{},
          attr<DisplayName>("Labels"),
          attr<Description>("Newline-separated list of node names, parallel with parents. Drawn inside "
                            "each rectangle that is large enough, and in the hover tooltip."),
          attr<Category>("Data")});
  proto->addField(
      "details"_rkey,
      field{
          std::string{},
          attr<DisplayName>("Details"),
          attr<Description>("Newline-separated list parallel with parents: extra text for each node's "
                            "hover tooltip, e.g. its formatted size."),
          attr<Category>("Data")});
  proto->addField(
      "selected"_rkey,
      field{
          int32_t{-1},
          attr<DisplayName>("Selected"),
          attr<Description>("Index of the node drawn with a highlight outline; -1 for none. "
                            "Application-owned: set it from a clicked handler."),
          attr<Category>("State")});
  proto->addField(
      "width"_rkey,
      field{
          float{-1.0f},
          attr<DisplayName>("Width"),
          attr<Description>("Width in pixels. 0 or negative fills the available width."),
          attr<Category>("Layout")});
  proto->addField(
      "height"_rkey,
      field{
          float{-1.0f},
          attr<DisplayName>("Height"),
          attr<Description>("Height in pixels. 0 or negative fills the available height."),
          attr<Category>("Layout")});
  proto->addField(
      "padding"_rkey,
      field{
          float{2.0f},
          attr<DisplayName>("Padding"),
          attr<Description>("Gap in pixels between a node's edge and its children."),
          attr<Category>("Layout"),
          attr<Range>(0, 16)});
  proto->addField(
      "headers"_rkey,
      field{
          bool{true},
          attr<DisplayName>("Headers"),
          attr<Description>("Reserve a title strip showing the label at the top of every node that has "
                            "children and is large enough."),
          attr<Category>("Appearance")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Treemap"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "A squarified treemap: a tree of weighted nodes drawn as nested rectangles whose areas are "
      "proportional to their sizes (disk usage, memory, budgets, ...). The tree is given as flat "
      "parallel arrays (parents, sizes, colors) plus newline-separated labels. Events: 'clicked' "
      "and 'activated' (double click), both {index: int32} naming the deepest node under the "
      "mouse."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_treemap>("wish"_key, "Treemap"_key));
}

} // namespace bdg::wish
