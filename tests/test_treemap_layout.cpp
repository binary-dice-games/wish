// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <ui/treemap_layout.hpp>

#include "src/bison/bison_object.hpp"

#include <cmath>
#include <vector>

using namespace bdg::bison;
namespace wish = bdg::wish;

namespace {

float area(const wish::treemap_cell& c) {
  return c.empty() ? 0.0f : c.width() * c.height();
}

bool overlap(const wish::treemap_cell& a, const wish::treemap_cell& b) {
  const float eps = 0.01f;
  return a.x0 < b.x1 - eps && b.x0 < a.x1 - eps && a.y0 < b.y1 - eps && b.y0 < a.y1 - eps;
}

wish::treemap_layout_options flat() {
  wish::treemap_layout_options o;
  o.padding = 0.0f;
  return o;
}

} // namespace

// ── Layout ────────────────────────────────────────────────────────────────────

TEST(TreemapLayout, EmptyInputYieldsNoCells) {
  EXPECT_TRUE(wish::layout_treemap({}, {}, 0, 0, 100, 100).empty());
}

TEST(TreemapLayout, RootFillsBounds) {
  auto cells = wish::layout_treemap({-1}, {5.0f}, 10, 20, 110, 70);
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_FLOAT_EQ(cells[0].x0, 10);
  EXPECT_FLOAT_EQ(cells[0].y0, 20);
  EXPECT_FLOAT_EQ(cells[0].x1, 110);
  EXPECT_FLOAT_EQ(cells[0].y1, 70);
  EXPECT_FALSE(cells[0].has_children);
}

TEST(TreemapLayout, ChildAreasAreProportionalAndTileTheParent) {
  const std::vector<int32_t> parents{-1, 0, 0, 0, 0};
  const std::vector<float> sizes{0.0f, 6.0f, 3.0f, 2.0f, 1.0f};
  auto cells = wish::layout_treemap(parents, sizes, 0, 0, 120, 100, flat());
  ASSERT_EQ(cells.size(), 5u);
  EXPECT_TRUE(cells[0].has_children);

  const float total = 120.0f * 100.0f;
  float sum = 0.0f;
  for (size_t i = 1; i < cells.size(); ++i) {
    EXPECT_NEAR(area(cells[i]), total * sizes[i] / 12.0f, total * 0.001f) << i;
    EXPECT_GE(cells[i].x0, -0.01f);
    EXPECT_GE(cells[i].y0, -0.01f);
    EXPECT_LE(cells[i].x1, 120.01f);
    EXPECT_LE(cells[i].y1, 100.01f);
    sum += area(cells[i]);
    for (size_t k = i + 1; k < cells.size(); ++k)
      EXPECT_FALSE(overlap(cells[i], cells[k])) << i << " overlaps " << k;
  }
  EXPECT_NEAR(sum, total, total * 0.001f);
}

// Squarifying is the point: equal weights must not come out as thin strips.
TEST(TreemapLayout, EqualChildrenStayCloseToSquare) {
  std::vector<int32_t> parents{-1};
  std::vector<float> sizes{0.0f};
  for (int i = 0; i < 16; ++i) {
    parents.push_back(0);
    sizes.push_back(1.0f);
  }
  auto cells = wish::layout_treemap(parents, sizes, 0, 0, 400, 400, flat());
  for (size_t i = 1; i < cells.size(); ++i) {
    const float ratio = cells[i].width() / cells[i].height();
    EXPECT_GT(ratio, 0.5f) << i;
    EXPECT_LT(ratio, 2.0f) << i;
  }
}

TEST(TreemapLayout, GrandchildrenStayInsideTheirParentWithPaddingAndHeader) {
  const std::vector<int32_t> parents{-1, 0, 0, 1, 1};
  const std::vector<float> sizes{0.0f, 3.0f, 1.0f, 2.0f, 1.0f};
  wish::treemap_layout_options o;
  o.padding = 3.0f;
  o.header_height = 16.0f;
  auto cells = wish::layout_treemap(parents, sizes, 0, 0, 400, 300, o);
  const auto& parent = cells[1];
  ASSERT_TRUE(parent.has_children);
  EXPECT_FLOAT_EQ(parent.header, 16.0f);
  for (size_t i : {size_t{3}, size_t{4}}) {
    ASSERT_FALSE(cells[i].empty()) << i;
    EXPECT_GE(cells[i].x0, parent.x0 + 3.0f - 0.01f);
    EXPECT_GE(cells[i].y0, parent.y0 + 16.0f - 0.01f);
    EXPECT_LE(cells[i].x1, parent.x1 - 3.0f + 0.01f);
    EXPECT_LE(cells[i].y1, parent.y1 - 3.0f + 0.01f);
  }
  // A leaf never reserves a header.
  EXPECT_FLOAT_EQ(cells[2].header, 0.0f);
  EXPECT_FALSE(cells[2].has_children);
}

TEST(TreemapLayout, ZeroSizedAndInvalidNodesAreEmpty) {
  // Node 2 has size 0; node 3 names a later node as its parent; node 4
  // hangs off the invalid node 3; node 5's size is missing.
  const std::vector<int32_t> parents{-1, 0, 0, 4, 3, 0};
  const std::vector<float> sizes{0.0f, 5.0f, 0.0f, 5.0f, 5.0f};
  auto cells = wish::layout_treemap(parents, sizes, 0, 0, 100, 100, flat());
  ASSERT_EQ(cells.size(), 6u);
  EXPECT_FALSE(cells[1].empty());
  EXPECT_TRUE(cells[2].empty());
  EXPECT_TRUE(cells[3].empty());
  EXPECT_TRUE(cells[4].empty());
  EXPECT_TRUE(cells[5].empty());
  // The only drawable child takes the whole root.
  EXPECT_NEAR(area(cells[1]), 100.0f * 100.0f, 1.0f);
}

TEST(TreemapLayout, NodeTooSmallToSubdivideKeepsItsChildrenEmpty) {
  const std::vector<int32_t> parents{-1, 0, 0, 2, 2};
  const std::vector<float> sizes{0.0f, 9999.0f, 1.0f, 1.0f, 1.0f};
  auto cells = wish::layout_treemap(parents, sizes, 0, 0, 100, 100, flat());
  ASSERT_FALSE(cells[2].empty());
  EXPECT_FALSE(cells[2].has_children);
  EXPECT_TRUE(cells[3].empty());
  EXPECT_TRUE(cells[4].empty());
}

// ── Hit test ──────────────────────────────────────────────────────────────────

TEST(TreemapLayout, HitTestReturnsTheDeepestNode) {
  const std::vector<int32_t> parents{-1, 0, 0, 1, 1};
  const std::vector<float> sizes{0.0f, 3.0f, 1.0f, 2.0f, 1.0f};
  wish::treemap_layout_options o;
  o.padding = 4.0f;
  auto cells = wish::layout_treemap(parents, sizes, 0, 0, 400, 300, o);

  auto center = [&](size_t i) {
    return std::pair<float, float>{(cells[i].x0 + cells[i].x1) * 0.5f, (cells[i].y0 + cells[i].y1) * 0.5f};
  };
  for (size_t leaf : {size_t{2}, size_t{3}, size_t{4}}) {
    auto [x, y] = center(leaf);
    EXPECT_EQ(wish::treemap_hit_test(cells, x, y), static_cast<int32_t>(leaf));
  }
  // The padding ring of node 1 belongs to node 1 itself, the root's to the root.
  EXPECT_EQ(wish::treemap_hit_test(cells, cells[1].x0 + 1.0f, cells[1].y0 + 1.0f), 1);
  EXPECT_EQ(wish::treemap_hit_test(cells, 1.0f, 1.0f), 0);
  EXPECT_EQ(wish::treemap_hit_test(cells, -5.0f, 10.0f), -1);
  EXPECT_EQ(wish::treemap_hit_test(cells, 500.0f, 10.0f), -1);
}

// ── Treemap element prototype ─────────────────────────────────────────────────

TEST(TreemapElement, IsRegisteredWithFillDefaults) {
  bdg::wish::register_all();
  auto obj = dynamic::instantiate("wish"_key, "Treemap"_key);
  ASSERT_NE(obj.findField(dynamic::CLASS), nullptr);
  EXPECT_EQ(obj.findField(dynamic::CLASS)->as<bdg::bison::key_t>(), "Treemap"_key);
  // The untyped findField() also resolves prototype defaults.
  for (const char* name : {"width", "height", "selected", "headers", "parents", "sizes", "colors", "labels"})
    ASSERT_NE(obj.findField(bdg::bison::key_t{name}), nullptr) << name;
  EXPECT_FLOAT_EQ(obj.findField("width"_key)->as<float>(), -1.0f);
  EXPECT_FLOAT_EQ(obj.findField("height"_key)->as<float>(), -1.0f);
  EXPECT_EQ(obj.findField("selected"_key)->as<int32_t>(), -1);
  EXPECT_TRUE(obj.findField("headers"_key)->as<bool>());
  EXPECT_TRUE(obj.findField("parents"_key)->is<std::vector<int32_t>>());
  EXPECT_TRUE(obj.findField("sizes"_key)->is<std::vector<float>>());
}
