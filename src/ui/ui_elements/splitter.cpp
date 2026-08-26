// MIT License © 2025 Binary Dice Games
/// @file splitter.cpp
/// @brief Registers the Splitter prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_splitter() {
  auto proto = dynamic_ptr{"Splitter"_rkey, {}};
  proto->addField(
      "orientation"_rkey,
      field{
          std::string{"vertical"},
          attr<DisplayName>("Orientation"),
          attr<Description>("\"vertical\" (default) draws a vertical drag bar and arranges children "
                            "side by side, resizing their \"width\" fields. \"horizontal\" draws a "
                            "horizontal drag bar and stacks children top to bottom, resizing their "
                            "\"height\" fields."),
          attr<Category>("Layout")});
  proto->addField(
      "thickness"_rkey,
      field{
          4.0f,
          attr<DisplayName>("Thickness"),
          attr<Description>("Pixel width (vertical orientation) or height (horizontal orientation) "
                            "of each draggable bar between panes."),
          attr<Category>("Layout"),
          attr<Range>(1, 64),
          attr<Step>(0.5)});
  proto->addField(
      "min_pane_size"_rkey,
      field{
          20.0f,
          attr<DisplayName>("Minimum Pane Size"),
          attr<Description>("Minimum pixel size any pane can be dragged down to."),
          attr<Category>("Layout"),
          attr<Range>(0, 4096),
          attr<Step>(1)});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Splitter"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Arranges its children as resizable panes separated by draggable bars "
                        "(imgui.com/issues/319's technique, built on the public InvisibleButton API). "
                        "All panes but the last get an explicit pixel size, persisted in that "
                        "child's own \"width\" (vertical orientation) or \"height\" (horizontal "
                        "orientation) field -- unset (0) panes split the available space evenly "
                        "on first render. The last pane always fills whatever space remains. "
                        "For 3+ panes, nest a Splitter inside another Splitter's last pane. "
                        "Emits 'resized' with {pane_index: int, size1: float, size2: float} "
                        "when a drag bar is released."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Layout"_key, dynamic::make_factory<ui_splitter>("wish"_key, "Splitter"_key));
}

} // namespace bdg::wish
