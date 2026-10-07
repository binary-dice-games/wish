// MIT License © 2026 Binary Dice Games
/// @file treemap_layout.hpp
/// @brief Squarified treemap layout for the Treemap element.
///
/// Pure geometry with no ImGui or bison dependency, so it can be unit
/// tested on its own; `render_treemap()` (src/imgui/imgui_treemap_renderer.cpp)
/// is its only production caller.
#pragma once

#include <cstdint>
#include <vector>

namespace bdg::wish {

/// @brief One node's rectangle, in the same coordinate space as the bounds
/// handed to layout_treemap().
struct treemap_cell {
  float x0{0.0f};
  float y0{0.0f};
  float x1{0.0f};
  float y1{0.0f};
  /// Height of the title strip reserved along the top of this node's
  /// rectangle for its label; 0 for a leaf, or for a parent too small to
  /// carry one.
  float header{0.0f};
  /// True when at least one child of this node got a non-empty rectangle.
  bool has_children{false};

  float width() const { return x1 - x0; }
  float height() const { return y1 - y0; }
  /// @brief True when the node was not laid out (zero size, an invalid
  /// parent, or no room left inside its parent).
  bool empty() const { return x1 <= x0 || y1 <= y0; }
  bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

/// @brief Layout parameters, in pixels.
struct treemap_layout_options {
  /// Gap between a parent's edge and its children.
  float padding{2.0f};
  /// Title strip reserved at the top of every parent that is large enough
  /// to carry one; 0 disables headers.
  float header_height{0.0f};
  /// A node narrower or shorter than this is drawn as a leaf: its children
  /// get empty cells.
  float min_parent_size{8.0f};
};

/// @brief Lays out a tree of weighted nodes as a squarified treemap
/// (Bruls, Huizing, van Wijk): each node's children tile its rectangle with
/// areas proportional to their sizes, kept as close to square as possible.
///
/// @param parents  `parents[i]` is the index of node i's parent; node 0 is
///                 the root and its entry is ignored. Every other entry must
///                 be smaller than its own index (a parent precedes its
///                 children) -- a node that breaks this is left empty, as is
///                 its whole subtree.
/// @param sizes    Node weights, parallel to @p parents. Only the ratio
///                 between siblings matters; a parent's own size is not
///                 compared with the sum of its children. Entries missing
///                 from a shorter array, and non-positive or non-finite
///                 ones, count as 0 (an empty cell).
/// @param x0,y0,x1,y1  The root's rectangle.
/// @return One cell per entry of @p parents, in the same order.
std::vector<treemap_cell> layout_treemap(
    const std::vector<int32_t>& parents, const std::vector<float>& sizes, float x0, float y0, float x1, float y1,
    const treemap_layout_options& options = {});

/// @brief Index of the deepest non-empty cell containing (@p x, @p y), or
/// -1 when the point is outside the root. @p cells must come from
/// layout_treemap() (children after their parent).
int32_t treemap_hit_test(const std::vector<treemap_cell>& cells, float x, float y);

} // namespace bdg::wish
