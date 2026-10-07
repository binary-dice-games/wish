// MIT License © 2026 Binary Dice Games
/// @file imgui_treemap_renderer.cpp
/// @brief ImGui render function for the Treemap element.
#include "imgui_treemap_renderer.hpp"

#ifdef WISH_IMGUI_ENABLED

#include "ui/treemap_layout.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

namespace {

struct rgb {
  int r, g, b;
};

/// Fill colors for nodes with no entry in `colors`, one per top-level branch.
constexpr rgb kPalette[] = {
    {86, 156, 214}, {106, 190, 120}, {220, 170, 80}, {200, 110, 110}, {150, 125, 210},
    {90, 190, 190}, {215, 135, 185}, {160, 175, 95}, {225, 140, 85},  {130, 150, 175},
};

/// @brief Unpacks a 0xRRGGBBAA int32 (see Treemap's `colors` field); alpha
/// is ignored, a treemap cell is always opaque.
rgb unpack_color(int32_t packed) {
  const auto u = static_cast<uint32_t>(packed);
  return {static_cast<int>((u >> 24) & 0xFFu), static_cast<int>((u >> 16) & 0xFFu), static_cast<int>((u >> 8) & 0xFFu)};
}

/// @brief @p c scaled towards white (@p f > 1) or black (@p f < 1).
ImU32 shade(rgb c, float f) {
  auto ch = [f](int v) {
    const float x = f <= 1.0f ? static_cast<float>(v) * f : 255.0f - (255.0f - static_cast<float>(v)) / f;
    return static_cast<int>(std::clamp(x, 0.0f, 255.0f));
  };
  return IM_COL32(ch(c.r), ch(c.g), ch(c.b), 255);
}

/// @brief Splits a newline-separated list into @p out without copying.
void split_lines(const std::string& text, std::vector<std::string_view>& out) {
  out.clear();
  if (text.empty())
    return;
  size_t begin = 0;
  while (true) {
    const size_t eol = text.find('\n', begin);
    if (eol == std::string::npos) {
      out.emplace_back(text.data() + begin, text.size() - begin);
      return;
    }
    out.emplace_back(text.data() + begin, eol - begin);
    begin = eol + 1;
  }
}

std::string_view line_at(const std::vector<std::string_view>& lines, size_t i) {
  return i < lines.size() ? lines[i] : std::string_view{};
}

} // namespace

void render_treemap(imgui_renderer&, const ui_element& node_base, const context& s) {
  const auto& node = static_cast<const ui_treemap&>(node_base);

  // A width/height <= 0 fills what is left -- see measure_treemap()
  // (imgui_layout.cpp) for the matching measure-pass rule.
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  float width = node.width(-1.0f);
  float height = node.height(-1.0f);
  if (width <= 0.0f)
    width = avail.x;
  if (height <= 0.0f)
    height = avail.y;
  width = std::max(width, 1.0f);
  height = std::max(height, 1.0f);

  // One item for the whole map: it reserves the space, gives
  // imgui_renderer::render_node() a rect to capture, and owns the mouse
  // interaction; the cells themselves are only drawn.
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##treemap", ImVec2(width, height));
  const bool hovered = ImGui::IsItemHovered();
  const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 corner(origin.x + width, origin.y + height);
  dl->AddRectFilled(origin, corner, ImGui::GetColorU32(ImGuiCol_FrameBg));

  static const std::vector<int32_t> kNoInts;
  static const std::vector<float> kNoFloats;
  const auto* parents_p = node.parents();
  const auto* sizes_p = node.sizes();
  const auto* colors_p = node.colors();
  const std::vector<int32_t>& parents = parents_p ? *parents_p : kNoInts;
  const std::vector<float>& sizes = sizes_p ? *sizes_p : kNoFloats;
  const std::vector<int32_t>& colors = colors_p ? *colors_p : kNoInts;
  if (parents.empty())
    return;

  const float font_size = ImGui::GetFontSize();
  treemap_layout_options options;
  options.padding = node.padding(2.0f);
  options.header_height = node.headers(true) ? font_size + 4.0f : 0.0f;
  const std::vector<treemap_cell> cells =
      layout_treemap(parents, sizes, origin.x, origin.y, corner.x, corner.y, options);

  // Scratch buffers reused across frames (render thread only).
  static thread_local std::vector<std::string_view> labels;
  static thread_local std::vector<std::string_view> details;
  static thread_local std::vector<int32_t> branch; // top-level ancestor of each node
  split_lines(node.labels_ref(), labels);
  split_lines(node.details_ref(), details);
  branch.assign(parents.size(), 0);
  for (size_t i = 1; i < parents.size(); ++i) {
    const int32_t p = parents[i];
    if (p >= 0 && static_cast<size_t>(p) < i)
      branch[i] = p == 0 ? static_cast<int32_t>(i) : branch[static_cast<size_t>(p)];
  }

  const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
  const ImU32 group_col = ImGui::GetColorU32(ImGuiCol_TitleBg);
  const ImU32 edge_col = IM_COL32(0, 0, 0, 120);
  ImFont* font = ImGui::GetFont();

  auto draw_label = [&](std::string_view text, const treemap_cell& c, float max_y, ImU32 col) {
    if (text.empty() || c.width() < font_size * 2.0f)
      return;
    const ImVec4 clip(c.x0 + 2.0f, c.y0, c.x1 - 2.0f, max_y);
    dl->AddText(
        font, font_size, ImVec2(c.x0 + 3.0f, c.y0 + 2.0f), col, text.data(), text.data() + text.size(), 0.0f, &clip);
  };

  for (size_t i = 0; i < cells.size(); ++i) {
    const treemap_cell& c = cells[i];
    if (c.empty() || c.width() < 1.0f || c.height() < 1.0f)
      continue;
    const ImVec2 lo(c.x0, c.y0);
    const ImVec2 hi(c.x1, c.y1);
    if (c.has_children) {
      // A group: a frame behind its children, titled when it has a header.
      dl->AddRectFilled(lo, hi, group_col);
      dl->AddRect(lo, hi, edge_col);
      if (c.header > 0.0f)
        draw_label(line_at(labels, i), c, c.y0 + c.header, text_col);
      continue;
    }
    const int32_t packed = i < colors.size() ? colors[i] : 0;
    const rgb base = packed != 0 ? unpack_color(packed)
                                 : kPalette[static_cast<size_t>(branch[i]) % std::size(kPalette)];
    // Lit from the top-left, like a cushion: reads as separate tiles even
    // when neighbours share a color.
    dl->AddRectFilledMultiColor(lo, hi, shade(base, 1.35f), shade(base, 1.0f), shade(base, 0.7f), shade(base, 1.0f));
    if (c.width() >= 3.0f && c.height() >= 3.0f)
      dl->AddRect(lo, hi, edge_col);
    if (c.height() >= font_size + 4.0f) {
      const bool dark = base.r * 299 + base.g * 587 + base.b * 114 < 140000;
      draw_label(line_at(labels, i), c, c.y1, dark ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255));
    }
  }

  auto outline = [&](int32_t index, ImU32 col) {
    if (index < 0 || static_cast<size_t>(index) >= cells.size() || cells[static_cast<size_t>(index)].empty())
      return;
    const treemap_cell& c = cells[static_cast<size_t>(index)];
    dl->AddRect(ImVec2(c.x0, c.y0), ImVec2(c.x1, c.y1), IM_COL32(0, 0, 0, 255), 0.0f, 0, 4.0f);
    dl->AddRect(ImVec2(c.x0, c.y0), ImVec2(c.x1, c.y1), col, 0.0f, 0, 2.0f);
  };
  outline(node.selected(-1), IM_COL32(255, 220, 60, 255));

  if (!hovered)
    return;
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const int32_t hit = treemap_hit_test(cells, mouse.x, mouse.y);
  if (hit < 0)
    return;
  outline(hit, IM_COL32(255, 255, 255, 255));

  // Tooltip: the node's path below the root, then its detail line.
  std::string tip;
  for (int32_t i = hit; i > 0; i = parents[static_cast<size_t>(i)]) {
    const std::string_view name = line_at(labels, static_cast<size_t>(i));
    tip.insert(0, tip.empty() ? std::string{name} : std::string{name} + "/");
  }
  if (tip.empty())
    tip = std::string{line_at(labels, 0)};
  if (const std::string_view detail = line_at(details, static_cast<size_t>(hit)); !detail.empty())
    tip += "\n" + std::string{detail};
  if (!tip.empty())
    ImGui::SetTooltip("%s", tip.c_str());

  // At most one of activated/clicked per frame, like Table's row events.
  dynamic payload;
  payload["index"_key] = hit;
  if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    enqueue_event(s, node.wish_id(), "activated"_key, std::move(payload));
  else if (clicked)
    enqueue_event(s, node.wish_id(), "clicked"_key, std::move(payload));
}

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
