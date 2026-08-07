// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/dev/git/server/git_graph_layout.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using bdg::wish::compute_git_graph_layout;
using bdg::wish::git_graph_commit_in;
using bdg::wish::git_graph_lane_color;
using bdg::wish::git_graph_row;

namespace {

// Finds the segment (if any) matching `from`->`to` in `segs`.
bool has_segment(const std::vector<bdg::wish::git_graph_segment>& segs, int32_t from, int32_t to) {
  for (auto& s : segs) {
    if (s.from_lane == from && s.to_lane == to)
      return true;
  }
  return false;
}

} // namespace

// ── Linear history ───────────────────────────────────────────────────────────
// C -> B -> A (C newest). Everything stays in lane 0.
TEST(GitGraphLayout, LinearHistoryStaysInLaneZero) {
  std::vector<git_graph_commit_in> commits{
      {"C", {"B"}},
      {"B", {"A"}},
      {"A", {}},
  };
  auto rows = compute_git_graph_layout(commits);
  ASSERT_EQ(rows.size(), 3u);
  for (auto& r : rows)
    EXPECT_EQ(r.lane, 0);

  // C's bottom connects straight down into B; B's top mirrors it.
  EXPECT_TRUE(has_segment(rows[0].bottom, 0, 0));
  EXPECT_TRUE(has_segment(rows[1].top, 0, 0));
  // A is a root commit: no bottom segments at all.
  EXPECT_TRUE(rows[2].bottom.empty());
}

// ── Simple branch + merge (the fixture used for manual/E2E verification) ─────
//
//   M (parents: S, F)      <- merge commit, lane 0
//   F (parent: S)          <- feature commit, lane 1 (new branch)
//   S (parent: I)          <- second commit, lane 0
//   I (parent: none)       <- initial commit, lane 0
//
// Matches `git log --oneline --graph --all` for:
//   *   merge feature
//   |\
//   | * feature commit
//   |/
//   * second commit
//   * initial commit
TEST(GitGraphLayout, BranchAndMergeDiamond) {
  std::vector<git_graph_commit_in> commits{
      {"M", {"S", "F"}},
      {"F", {"S"}},
      {"S", {"I"}},
      {"I", {}},
  };
  auto rows = compute_git_graph_layout(commits);
  ASSERT_EQ(rows.size(), 4u);

  // M (row 0): own lane 0; bottom has a straight line to S (lane 0) and a
  // diverging line out to F's new lane (1).
  EXPECT_EQ(rows[0].lane, 0);
  EXPECT_TRUE(has_segment(rows[0].bottom, 0, 0));
  EXPECT_TRUE(has_segment(rows[0].bottom, 0, 1));

  // F (row 1): lane 1; M's diverging segment (0->1) already performed its
  // lane change within row 0's own curve, so it arrives here collapsed to a
  // straight top pass-through (1->1) rather than a duplicated diagonal. Its
  // own line to S also stays straight (1->1) at this row -- the convergence
  // into lane 0 only happens at the row S itself appears (row 2), the
  // latest possible point, matching git log --graph's own rendering.
  EXPECT_EQ(rows[1].lane, 1);
  EXPECT_TRUE(has_segment(rows[1].top, 1, 1));
  EXPECT_TRUE(has_segment(rows[1].bottom, 1, 1));

  // S (row 2): back in lane 0; both M's straight-through (0->0, from row 0's
  // bottom) and F's now-converging line (1->0, redirected from row 1's
  // straight 1->1 pass-through) land in its top half.
  EXPECT_EQ(rows[2].lane, 0);
  EXPECT_TRUE(has_segment(rows[2].top, 0, 0));
  EXPECT_TRUE(has_segment(rows[2].top, 1, 0));
  EXPECT_TRUE(has_segment(rows[2].bottom, 0, 0));

  // I (row 3): root commit, lane 0, no bottom segments.
  EXPECT_EQ(rows[3].lane, 0);
  EXPECT_TRUE(rows[3].bottom.empty());
}

// ── Octopus merge (three parents) ─────────────────────────────────────────────
TEST(GitGraphLayout, OctopusMergeAllocatesOneLanePerExtraParent) {
  std::vector<git_graph_commit_in> commits{
      {"M", {"A", "B", "C"}},
      {"A", {}},
      {"B", {}},
      {"C", {}},
  };
  auto rows = compute_git_graph_layout(commits);
  ASSERT_EQ(rows.size(), 4u);

  EXPECT_EQ(rows[0].lane, 0);
  // First parent stays in M's own lane; the other two each get a fresh lane.
  EXPECT_TRUE(has_segment(rows[0].bottom, 0, 0));
  EXPECT_TRUE(has_segment(rows[0].bottom, 0, 1));
  EXPECT_TRUE(has_segment(rows[0].bottom, 0, 2));

  // A/B/C occupy three distinct lanes, in some order matching allocation.
  std::vector<int32_t> lanes{rows[1].lane, rows[2].lane, rows[3].lane};
  std::sort(lanes.begin(), lanes.end());
  EXPECT_EQ(lanes, (std::vector<int32_t>{0, 1, 2}));
}

// ── Empty input ────────────────────────────────────────────────────────────
TEST(GitGraphLayout, EmptyInputProducesNoRows) {
  EXPECT_TRUE(compute_git_graph_layout({}).empty());
}

// ── Color determinism ────────────────────────────────────────────────────────
TEST(GitGraphLayout, LaneColorIsDeterministicAndCycles) {
  EXPECT_EQ(git_graph_lane_color(0), git_graph_lane_color(0));
  EXPECT_NE(git_graph_lane_color(0), git_graph_lane_color(1));
  // Palette has 8 entries -- lane 8 must cycle back to lane 0's color.
  EXPECT_EQ(git_graph_lane_color(0), git_graph_lane_color(8));
}
