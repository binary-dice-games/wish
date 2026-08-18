// MIT License © 2025 Binary Dice Games
/// @file spring.cpp
/// @brief Registers the Spring prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_spring() {
  auto proto = dynamic_ptr{"Spring"_rkey, {}};
  proto->addField(
      "weight"_rkey,
      field{
          1.0f,
          attr<DisplayName>("Weight"),
          attr<Description>("Expandable, invisible space as a direct child of a HorizontalLayout "
                            "(width axis) or VerticalLayout (height axis): shares whatever room is "
                            "left over in that layout's stretch pool among sibling stretch children "
                            "(Layout.width/height < 0, and other Springs), weighted by this value -- "
                            "the same weighted-share convention as Layout.width/height's negative-value "
                            "mode (mirrors ImGuiTableColumnFlags_WidthStretch). Two Springs around one "
                            "child centers it; a Spring between two children pushes them to opposite "
                            "edges (\"space-between\"); unequal weights bias the split. A value of 0 "
                            "claims no share at all (the Spring collapses, flushing its neighbor to "
                            "that edge -- CSS flex-grow's convention), the same as omitting it entirely "
                            "if every other sibling Spring is also 0. Negative values clamp to 0. "
                            "Ignored (renders as zero-size) outside a HorizontalLayout or VerticalLayout."),
          attr<Category>("Layout")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Spring"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Expandable space that shares leftover room with sibling stretch children."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Spring"_key));
}

} // namespace bdg::wish
