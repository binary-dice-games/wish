// MIT License © 2025 Binary Dice Games
/// @file graph_node.cpp
/// @brief Registers the GraphNode prototype.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_graph_node() {
  auto proto = dynamic_ptr{"GraphNode"_rkey, {}};
  proto->addField(
      "lane"_rkey,
      field{
          int32_t{0},
          attr<DisplayName>("Lane"),
          attr<Description>("This row's own dot column (0-based)."),
          attr<Category>("Data")});
  proto->addField(
      "color"_rkey,
      field{
          int32_t{0},
          attr<DisplayName>("Color"),
          attr<Description>("Packed 0xRRGGBBAA color for this row's own dot."),
          attr<Category>("Data")});
  proto->addField(
      "is_head"_rkey,
      field{
          bool{false},
          attr<DisplayName>("Is HEAD"),
          attr<Description>("Draws a ring around the dot marking the current HEAD commit."),
          attr<Category>("Data")});
  proto->addField(
      "is_working"_rkey,
      field{
          bool{false},
          attr<DisplayName>("Is Working"),
          attr<Description>("Draws a hollow dot for the synthetic \"uncommitted changes\" row."),
          attr<Category>("Data")});
  proto->addField(
      "top_from"_rkey,
      field{
          std::vector<int32_t>{},
          attr<DisplayName>("Top Segment: From Lane"),
          attr<Description>("Parallel array with top_to/top_color: one entry per line segment drawn "
                            "in this row's top half (row top to row center), each segment's "
                            "starting lane."),
          attr<Category>("Data")});
  proto->addField(
      "top_to"_rkey,
      field{
          std::vector<int32_t>{},
          attr<DisplayName>("Top Segment: To Lane"),
          attr<Description>("Parallel array with top_from/top_color: each segment's ending lane at "
                            "the row's vertical center."),
          attr<Category>("Data")});
  proto->addField(
      "top_color"_rkey,
      field{
          std::vector<int32_t>{},
          attr<DisplayName>("Top Segment: Color"),
          attr<Description>("Parallel array with top_from/top_to: each segment's packed 0xRRGGBBAA color."),
          attr<Category>("Data")});
  proto->addField(
      "bottom_from"_rkey,
      field{
          std::vector<int32_t>{},
          attr<DisplayName>("Bottom Segment: From Lane"),
          attr<Description>("Parallel array with bottom_to/bottom_color: one entry per line segment "
                            "drawn in this row's bottom half (row center to row bottom), each "
                            "segment's starting lane at the row's vertical center."),
          attr<Category>("Data")});
  proto->addField(
      "bottom_to"_rkey,
      field{
          std::vector<int32_t>{},
          attr<DisplayName>("Bottom Segment: To Lane"),
          attr<Description>("Parallel array with bottom_from/bottom_color: each segment's ending lane."),
          attr<Category>("Data")});
  proto->addField(
      "bottom_color"_rkey,
      field{
          std::vector<int32_t>{},
          attr<DisplayName>("Bottom Segment: Color"),
          attr<Description>("Parallel array with bottom_from/bottom_to: each segment's packed 0xRRGGBBAA color.")});
  proto->addField(
      "lane_width"_rkey,
      field{
          16.0f,
          attr<DisplayName>("Lane Width"),
          attr<Description>("Pixel spacing between adjacent lanes."),
          attr<Category>("Layout"),
          attr<Range>(4, 64)});
  proto->addField(
      "dot_radius"_rkey,
      field{
          4.5f,
          attr<DisplayName>("Dot Radius"),
          attr<Description>("Commit dot radius in pixels."),
          attr<Category>("Layout"),
          attr<Range>(1, 16)});
  proto->addField(
      "row_height"_rkey,
      field{
          0.0f,
          attr<DisplayName>("Row Height"),
          attr<Description>("Total row height in pixels. 0 uses one text line height with spacing, "
                            "matching an adjacent text cell's natural row height."),
          attr<Category>("Layout")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("GraphNode"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Draws one row's local segment of a lane-based DAG graph (a commit dot plus every line "
      "segment touching that row): its own dot, and lane-to-lane connector segments split into a "
      "top half (row top to row center) and bottom half (row center to row bottom). Meant to sit "
      "as the leftmost cell of a Table row, so scrolling and row selection come from the "
      "surrounding Table for free -- this element only draws, it emits no events of its own. Not "
      "specific to git: any lane-assigned DAG (branch/merge graphs, dependency graphs, ...) can "
      "drive it from the same field shape."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key,
      dynamic::make_factory<ui_graph_node>("wish"_key, "GraphNode"_key));
}

} // namespace bdg::wish
