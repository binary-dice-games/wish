// MIT License © 2025 Binary Dice Games
/// @file imgui_graph_renderer.cpp
/// @brief ImGui render function for the GraphNode element.
#include "imgui_graph_renderer.hpp"

#ifdef WISH_IMGUI_ENABLED

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

namespace {

const std::vector<int32_t>* i32_vec_field(const dynamic& obj, key_t k) {
  const auto* f = obj.findField(k);
  return (f && f->is<std::vector<int32_t>>()) ? &f->as<std::vector<int32_t>>() : nullptr;
}

/// @brief Unpacks a 0xRRGGBBAA int32 (see GraphNode's field docs/
/// docs/ui-elements.md) into an ImGui-ready ImU32, going through the
/// IM_COL32(r,g,b,a) macro rather than assuming ImGui's internal channel
/// byte order.
ImU32 unpack_color(int32_t packed) {
  const auto u = static_cast<uint32_t>(packed);
  const auto r = static_cast<int>((u >> 24) & 0xFFu);
  const auto g = static_cast<int>((u >> 16) & 0xFFu);
  const auto b = static_cast<int>((u >> 8) & 0xFFu);
  const auto a = static_cast<int>(u & 0xFFu);
  return IM_COL32(r, g, b, a);
}

} // namespace

void render_graph_node(imgui_renderer&, const ui_element& node, const context&) {
  const int32_t lane = node.get_as<int32_t>("lane"_key, 0);
  const int32_t color = node.get_as<int32_t>("color"_key, 0);
  const bool is_head = node.get_as<bool>("is_head"_key, false);
  const bool is_working = node.get_as<bool>("is_working"_key, false);
  const float lane_width = node.get_as<float>("lane_width"_key, 16.0f);
  const float dot_radius = node.get_as<float>("dot_radius"_key, 4.5f);
  float row_height = node.get_as<float>("row_height"_key, 0.0f);
  if (row_height <= 0.0f)
    row_height = ImGui::GetTextLineHeightWithSpacing();

  const auto* top_from = i32_vec_field(node, "top_from"_key);
  const auto* top_to = i32_vec_field(node, "top_to"_key);
  const auto* top_color = i32_vec_field(node, "top_color"_key);
  const auto* bottom_from = i32_vec_field(node, "bottom_from"_key);
  const auto* bottom_to = i32_vec_field(node, "bottom_to"_key);
  const auto* bottom_color = i32_vec_field(node, "bottom_color"_key);

  // Reserve at least enough width for every lane any segment (or this row's
  // own dot) touches, so a row whose lines fan out further than its own dot
  // (a branch-out/merge-in diagonal) doesn't get clipped by a too-narrow cell.
  int32_t max_lane = lane;
  auto note_max = [&](const std::vector<int32_t>* v) {
    if (!v)
      return;
    for (int32_t l : *v)
      max_lane = std::max(max_lane, l);
  };
  note_max(top_from);
  note_max(top_to);
  note_max(bottom_from);
  note_max(bottom_to);

  const float width = (static_cast<float>(max_lane) + 1.0f) * lane_width + dot_radius * 2.0f;

  // Dummy() both reserves this row's cell space (so the surrounding Table
  // column sizes correctly) and gives GetItemRectMin/Max() a real item, same
  // as every other leaf widget -- see imgui_renderer::render_node()'s
  // per-element rect capture.
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(width, row_height));

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float top_y = origin.y;
  const float mid_y = origin.y + row_height * 0.5f;
  const float bot_y = origin.y + row_height;
  const auto lane_x = [&](int32_t l) { return origin.x + dot_radius + static_cast<float>(l) * lane_width; };

  const auto draw_segments = [&](
      const std::vector<int32_t>* from,
      const std::vector<int32_t>* to,
      const std::vector<int32_t>* col,
      float y0,
      float y1) {
    if (!from || !to || !col)
      return;
    const size_t n = std::min({from->size(), to->size(), col->size()});
    for (size_t i = 0; i < n; ++i) {
      const float x0 = lane_x((*from)[i]);
      const float x1 = lane_x((*to)[i]);
      const ImU32 c = unpack_color((*col)[i]);
      if ((*from)[i] == (*to)[i]) {
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), c, 2.0f);
      } else {
        // Lane-changing segment (branch-out/merge-in): a smooth S-curve
        // between the two lanes, control points held at the segment's own
        // start/end lane so the curve stays flat at both endpoints --
        // the same shape git log --graph/gitk/magit render for these.
        dl->AddBezierCubic(
            ImVec2(x0, y0), ImVec2(x0, (y0 + y1) * 0.5f), ImVec2(x1, (y0 + y1) * 0.5f), ImVec2(x1, y1), c, 2.0f);
      }
    }
  };

  draw_segments(top_from, top_to, top_color, top_y, mid_y);
  draw_segments(bottom_from, bottom_to, bottom_color, mid_y, bot_y);

  const ImVec2 dot_center(lane_x(lane), mid_y);
  const ImU32 dot_col = unpack_color(color);
  if (is_working)
    dl->AddCircle(dot_center, dot_radius, dot_col, 0, 2.0f);
  else
    dl->AddCircleFilled(dot_center, dot_radius, dot_col);
  if (is_head)
    dl->AddCircle(dot_center, dot_radius + 2.5f, dot_col, 0, 1.5f);
}

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
