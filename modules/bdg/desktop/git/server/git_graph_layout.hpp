// MIT License © 2025 Binary Dice Games
/// @file git_graph_layout.hpp
/// @brief Pure lane-assignment algorithm turning a commit/parent DAG into
///        per-row GraphNode field data (see src/ui/ui_elements/graph_node.cpp).
///
/// No UI/bison dependency -- unit-testable in isolation (tests/test_git_graph_layout.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief One commit's identity/parentage, in the order it should be
/// displayed (newest first, topologically sorted -- a commit must never
/// appear before any of its children in this order).
struct git_graph_commit_in {
  std::string hash;
  std::vector<std::string> parents; // full hashes; first parent first.
};

/// @brief One lane-to-lane connector segment, matching GraphNode's
/// top_from/top_to/top_color (or bottom_*) parallel-array field shape.
struct git_graph_segment {
  int32_t from_lane{0};
  int32_t to_lane{0};
  int32_t color{0}; // packed 0xRRGGBBAA
};

/// @brief One commit's computed lane/dot color plus its top-half (row top to
/// row center) and bottom-half (row center to row bottom) connector segments
/// -- directly assignable to a GraphNode's fields.
struct git_graph_row {
  int32_t lane{0};
  int32_t color{0};
  std::vector<git_graph_segment> top;
  std::vector<git_graph_segment> bottom;
};

/// @brief Assigns a lane, dot color, and connector segments to each entry in
/// @p commits, using the standard `git log --graph`/gitk/magit technique: a
/// list of "active lanes" is tracked, each holding the hash it next expects
/// to see; commits are walked in display order, each either continuing an
/// existing lane (its hash was expected) or starting a fresh one, freeing/
/// reassigning lanes according to its parents. Every other lane still active
/// (not this row's own) gets a straight pass-through segment so parallel
/// branches read as continuous lines. A commit's own line into its parent(s)
/// becomes this row's bottom half; any other lane also waiting for this same
/// hash (a shared descendant of two visible branch tips) has its last
/// pass-through segment redirected to converge into this row's dot.
///
/// @return One `git_graph_row` per entry in @p commits, same order/length.
std::vector<git_graph_row> compute_git_graph_layout(const std::vector<git_graph_commit_in>& commits);

/// @brief Deterministic lane → dot/line color mapping (cycles through a
/// fixed palette). Exposed so callers needing a color for a lane outside the
/// layout itself (e.g. the synthetic "Uncommitted changes" row's connector
/// down into HEAD's lane) can stay visually consistent with it.
int32_t git_graph_lane_color(int32_t lane);

} // namespace bdg::wish
