// MIT License © 2025 Binary Dice Games
/// @file git_graph_layout.cpp
/// @brief Implementation of compute_git_graph_layout().
#include "git_graph_layout.hpp"

#include <cstddef>

namespace bdg::wish {

namespace {

// Packed 0xRRGGBBAA. A fixed, visually distinct palette cycled by lane index
// -- deterministic so the same repo/lane always renders the same color
// across refreshes, matching gitk/SourceTree's own stable-per-lane coloring.
constexpr int32_t kPalette[] = {
    static_cast<int32_t>(0xE06C75FFU), // red
    static_cast<int32_t>(0x61AFEFFFU), // blue
    static_cast<int32_t>(0x98C379FFU), // green
    static_cast<int32_t>(0xE5C07BFFU), // yellow
    static_cast<int32_t>(0xC678DDFFU), // purple
    static_cast<int32_t>(0x56B6C2FFU), // cyan
    static_cast<int32_t>(0xD19A66FFU), // orange
    static_cast<int32_t>(0xABB2BFFFU), // gray
};
constexpr size_t kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

} // namespace

int32_t git_graph_lane_color(int32_t lane) {
  if (lane < 0)
    lane = 0;
  return kPalette[static_cast<size_t>(lane) % kPaletteSize];
}

std::vector<git_graph_row> compute_git_graph_layout(const std::vector<git_graph_commit_in>& commits) {
  std::vector<git_graph_row> rows(commits.size());
  std::vector<std::string> active_lanes; // "" == free; else the hash this lane is waiting for.

  auto find_free_lane = [&]() -> int32_t {
    for (size_t j = 0; j < active_lanes.size(); ++j) {
      if (active_lanes[j].empty())
        return static_cast<int32_t>(j);
    }
    active_lanes.emplace_back();
    return static_cast<int32_t>(active_lanes.size() - 1);
  };

  for (size_t i = 0; i < commits.size(); ++i) {
    const auto& commit = commits[i];
    git_graph_row& row = rows[i];

    // 1. Find (or allocate) this commit's own lane.
    int32_t lane = -1;
    for (size_t j = 0; j < active_lanes.size(); ++j) {
      if (active_lanes[j] == commit.hash) {
        lane = static_cast<int32_t>(j);
        break;
      }
    }
    if (lane < 0)
      lane = find_free_lane();

    row.lane = lane;
    row.color = git_graph_lane_color(lane);

    // 2. Top half mirrors the previous row's bottom half -- adjacent rows
    // share a border, so whatever line crossed it continues unchanged.
    row.top = (i > 0) ? rows[i - 1].bottom : std::vector<git_graph_segment>{};

    // 3. Any OTHER lane also waiting for this exact hash (two visible branch
    // tips sharing a descendant) converges into this row's dot: free it, and
    // bend its last straight pass-through (guaranteed present by induction --
    // every row an active lane survives copies its {j,j} pass-through
    // forward via step 2) so it lands on `lane` instead of running past it.
    for (size_t j = 0; j < active_lanes.size(); ++j) {
      if (static_cast<int32_t>(j) == lane || active_lanes[j] != commit.hash)
        continue;
      active_lanes[j] = "";
      for (auto& seg : row.top) {
        if (seg.from_lane == static_cast<int32_t>(j) && seg.to_lane == static_cast<int32_t>(j)) {
          seg.to_lane = lane;
          break;
        }
      }
    }

    // 4. Bottom half: a straight pass-through for every other still-active
    // lane (parallel branches continuing past this row unchanged), plus this
    // commit's own line(s) into its parent(s).
    for (size_t j = 0; j < active_lanes.size(); ++j) {
      if (static_cast<int32_t>(j) != lane && !active_lanes[j].empty())
        row.bottom.push_back({static_cast<int32_t>(j), static_cast<int32_t>(j), git_graph_lane_color(int32_t(j))});
    }

    if (commit.parents.empty()) {
      // Root commit: this lane ends here, free for a later row to reuse.
      active_lanes[static_cast<size_t>(lane)] = "";
    } else {
      active_lanes[static_cast<size_t>(lane)] = commit.parents[0];
      row.bottom.push_back({lane, lane, row.color});
      for (size_t k = 1; k < commit.parents.size(); ++k) {
        const int32_t new_lane = find_free_lane();
        active_lanes[static_cast<size_t>(new_lane)] = commit.parents[k];
        row.bottom.push_back({lane, new_lane, git_graph_lane_color(new_lane)});
      }
    }
  }

  return rows;
}

} // namespace bdg::wish
