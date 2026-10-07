// MIT License © 2026 Binary Dice Games
/// @file treemap_layout.cpp
/// @brief Squarified treemap layout for the Treemap element.
#include "treemap_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace bdg::wish {

namespace {

/// @brief Worst aspect ratio in a row of total area @p sum laid along a
/// side of length @p side, whose largest and smallest items have areas
/// @p largest and @p smallest.
double worst_ratio(double sum, double largest, double smallest, double side) {
  const double s2 = side * side;
  const double sum2 = sum * sum;
  return std::max(s2 * largest / sum2, sum2 / (s2 * smallest));
}

/// @brief Tiles @p items (child indices, largest first, all with a positive
/// size) into the rectangle, writing each one's cell.
void squarify(
    const std::vector<int32_t>& items, const std::vector<float>& sizes, double x, double y, double w, double h,
    std::vector<treemap_cell>& cells) {
  double total = 0.0;
  for (int32_t i : items)
    total += sizes[static_cast<size_t>(i)];
  if (total <= 0.0 || w <= 0.0 || h <= 0.0)
    return;
  const double scale = w * h / total; // size -> area in px^2
  auto area = [&](size_t k) { return sizes[static_cast<size_t>(items[k])] * scale; };

  size_t begin = 0;
  while (begin < items.size() && w > 0.0 && h > 0.0) {
    const double side = std::min(w, h);
    // Grow the row while doing so does not worsen its worst aspect ratio.
    double sum = area(begin);
    double worst = worst_ratio(sum, area(begin), area(begin), side);
    size_t end = begin + 1;
    while (end < items.size()) {
      const double next_sum = sum + area(end);
      const double next_worst = worst_ratio(next_sum, area(begin), area(end), side);
      if (next_worst > worst)
        break;
      sum = next_sum;
      worst = next_worst;
      ++end;
    }

    // The row takes a strip along the shorter side; the last row takes
    // whatever is left, so rounding never leaves a sliver uncovered.
    const bool last = end == items.size();
    const bool vertical = w >= h; // a column at the left, items stacked
    const double thickness = last ? (vertical ? w : h) : sum / side;
    double offset = 0.0;
    for (size_t k = begin; k < end; ++k) {
      const double length = k + 1 == end ? side - offset : area(k) / thickness;
      treemap_cell& c = cells[static_cast<size_t>(items[k])];
      if (vertical) {
        c.x0 = static_cast<float>(x);
        c.x1 = static_cast<float>(x + thickness);
        c.y0 = static_cast<float>(y + offset);
        c.y1 = static_cast<float>(y + offset + length);
      } else {
        c.y0 = static_cast<float>(y);
        c.y1 = static_cast<float>(y + thickness);
        c.x0 = static_cast<float>(x + offset);
        c.x1 = static_cast<float>(x + offset + length);
      }
      offset += length;
    }
    if (vertical) {
      x += thickness;
      w -= thickness;
    } else {
      y += thickness;
      h -= thickness;
    }
    begin = end;
  }
}

} // namespace

std::vector<treemap_cell> layout_treemap(
    const std::vector<int32_t>& parents, const std::vector<float>& sizes_in, float x0, float y0, float x1, float y1,
    const treemap_layout_options& options) {
  const size_t n = parents.size();
  std::vector<treemap_cell> cells(n);
  if (n == 0 || x1 <= x0 || y1 <= y0)
    return cells;

  std::vector<float> sizes(n, 0.0f);
  for (size_t i = 0; i < n && i < sizes_in.size(); ++i)
    if (std::isfinite(sizes_in[i]) && sizes_in[i] > 0.0f)
      sizes[i] = sizes_in[i];

  // Children of each node, as one flat array grouped by parent (a counting
  // sort), so a large tree costs two allocations rather than one per node.
  auto valid_child = [&](size_t i) {
    return i > 0 && parents[i] >= 0 && static_cast<size_t>(parents[i]) < i && sizes[i] > 0.0f;
  };
  std::vector<size_t> first(n + 1, 0);
  for (size_t i = 1; i < n; ++i)
    if (valid_child(i))
      ++first[static_cast<size_t>(parents[i]) + 1];
  for (size_t i = 0; i < n; ++i)
    first[i + 1] += first[i];
  std::vector<int32_t> children(first[n]);
  {
    std::vector<size_t> next(first.begin(), first.end() - 1);
    for (size_t i = 1; i < n; ++i)
      if (valid_child(i))
        children[next[static_cast<size_t>(parents[i])]++] = static_cast<int32_t>(i);
  }

  cells[0].x0 = x0;
  cells[0].y0 = y0;
  cells[0].x1 = x1;
  cells[0].y1 = y1;

  std::vector<int32_t> items;
  // Index order visits a parent before its children, so each node's own
  // cell is final by the time its children are tiled into it.
  for (size_t i = 0; i < n; ++i) {
    treemap_cell& c = cells[i];
    if (first[i] == first[i + 1] || c.empty())
      continue;
    if (c.width() < options.min_parent_size || c.height() < options.min_parent_size)
      continue;

    const float pad = std::max(options.padding, 0.0f);
    float header = 0.0f;
    if (options.header_height > 0.0f && c.height() >= options.header_height * 2.0f + pad &&
        c.width() >= options.header_height * 2.0f)
      header = options.header_height;
    const double ix = c.x0 + pad;
    const double iy = c.y0 + (header > 0.0f ? header : pad);
    const double iw = c.x1 - pad - ix;
    const double ih = c.y1 - pad - iy;
    if (iw < 1.0 || ih < 1.0)
      continue;

    items.assign(children.begin() + static_cast<ptrdiff_t>(first[i]), children.begin() + static_cast<ptrdiff_t>(first[i + 1]));
    std::stable_sort(items.begin(), items.end(), [&](int32_t a, int32_t b) {
      return sizes[static_cast<size_t>(a)] > sizes[static_cast<size_t>(b)];
    });
    squarify(items, sizes, ix, iy, iw, ih, cells);
    c.header = header;
    c.has_children = true;
  }
  return cells;
}

int32_t treemap_hit_test(const std::vector<treemap_cell>& cells, float x, float y) {
  // Siblings never overlap and children follow their parent, so the last
  // cell containing the point is the deepest one.
  for (size_t i = cells.size(); i-- > 0;)
    if (!cells[i].empty() && cells[i].contains(x, y))
      return static_cast<int32_t>(i);
  return -1;
}

} // namespace bdg::wish
