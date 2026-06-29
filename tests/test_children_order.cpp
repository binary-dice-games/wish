// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <registry.hpp>
#include <ui_element.hpp>
#include <ui_importer.hpp>
#include "src/bison/bison_object.hpp"

#include <string>
#include <vector>

using namespace bdg::bison;
using bdg::wish::ui_element;

class ChildrenOrderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }

  // Collect visited child keys from for_each_child_ordered into a vector.
  static std::vector<bdg::bison::key_t> collect_order(ui_element& parent) {
    std::vector<bdg::bison::key_t> keys;
    parent.for_each_child_ordered([&](bdg::bison::key_t k, ui_element&) { keys.push_back(k); });
    return keys;
  }
};

// ── Named children in hash-adversarial declaration order ──────────────────────

// Keys "zzz", "aaa", "mmm" hash to different values; without the order cache
// they would be visited in hash-sorted order, not declaration order.
TEST_F(ChildrenOrderTest, NamedChildrenRespectDeclarationOrder) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": {
      "zzz": { "type": "Label", "text": "first" },
      "aaa": { "type": "Label", "text": "second" },
      "mmm": { "type": "Label", "text": "third" }
    }
  })";

  auto result = bdg::wish::import_json(desc);
  auto& win = result[""];
  ASSERT_NE(win, nullptr);

  auto keys = collect_order(*win);
  ASSERT_EQ(keys.size(), 3u);
  EXPECT_EQ(keys[0], "zzz"_key);
  EXPECT_EQ(keys[1], "aaa"_key);
  EXPECT_EQ(keys[2], "mmm"_key);
}

// ── Indexed children preserve numeric order ───────────────────────────────────

TEST_F(ChildrenOrderTest, IndexedChildrenPreserveOrder) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": [
      { "type": "Label", "text": "first" },
      { "type": "Label", "text": "second" },
      { "type": "Label", "text": "third" }
    ]
  })";

  auto result = bdg::wish::import_json(desc);
  auto& win = result[""];
  ASSERT_NE(win, nullptr);

  std::vector<std::string> texts;
  win->for_each_child_ordered(
      [&](bdg::bison::key_t, ui_element& child) { texts.push_back(child.findField("text"_key)->as<std::string>()); });

  ASSERT_EQ(texts.size(), 3u);
  EXPECT_EQ(texts[0], "first");
  EXPECT_EQ(texts[1], "second");
  EXPECT_EQ(texts[2], "third");
}

// ── Explicit order override ───────────────────────────────────────────────────

// Declare children in alphabetical order but use explicit 'order' values to
// reverse them.  Verify the renderer sees the user-specified sequence.
TEST_F(ChildrenOrderTest, ExplicitOrderFieldOverridesDeclarationOrder) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": {
      "aaa": { "type": "Label", "text": "second", "order": 1 },
      "zzz": { "type": "Label", "text": "first",  "order": 0 }
    }
  })";

  auto result = bdg::wish::import_json(desc);
  auto& win = result[""];
  ASSERT_NE(win, nullptr);

  auto keys = collect_order(*win);
  ASSERT_EQ(keys.size(), 2u);
  EXPECT_EQ(keys[0], "zzz"_key); // order 0 → rendered first
  EXPECT_EQ(keys[1], "aaa"_key); // order 1 → rendered second
}

// ── refresh_children_order after runtime mutation ─────────────────────────────

TEST_F(ChildrenOrderTest, RefreshAfterOrderMutation) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": {
      "first":  { "type": "Label", "text": "first" },
      "second": { "type": "Label", "text": "second" }
    }
  })";

  auto result = bdg::wish::import_json(desc);
  auto& win = result[""];
  ASSERT_NE(win, nullptr);

  // Initial order: first → second.
  {
    auto keys = collect_order(*win);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "first"_key);
    EXPECT_EQ(keys[1], "second"_key);
  }

  // Mutate: give "second" a lower order value so it renders first.
  auto* children_field = win->findField("children"_key);
  ASSERT_NE(children_field, nullptr);
  auto& children = children_field->as<dynamic_ptr>();
  auto* second_field = children->findField("second"_key);
  ASSERT_NE(second_field, nullptr);
  auto& second = second_field->as<dynamic_ptr>();
  (*second)["order"_key] = int32_t{-1};

  // Cache is stale — collect_order still returns the old order.
  {
    auto keys = collect_order(*win);
    EXPECT_EQ(keys[0], "first"_key);
  }

  // After refresh the new order is reflected.
  win->refresh_children_order();
  {
    auto keys = collect_order(*win);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "second"_key); // order -1 → now first
    EXPECT_EQ(keys[1], "first"_key);
  }
}

// ── No-cache fallback ─────────────────────────────────────────────────────────

// for_each_child_ordered on a manually-constructed parent (no cache built)
// falls back to forEachChild<ui_element> without crashing.
TEST_F(ChildrenOrderTest, FallbackWhenNoCachePresent) {
  auto win = dynamic::instantiate<ui_element>("wish"_key, "Window"_key);
  auto children = dynamic_ptr{bdg::bison::key_t{0U}, {}};
  auto lbl = dynamic::instantiate<ui_element>("wish"_key, "Label"_key);
  (*children)["x"_key] = lbl;
  (*win)["children"_key] = children;

  // Should not crash and should visit the one child.
  int count = 0;
  win->for_each_child_ordered([&](bdg::bison::key_t, ui_element&) { ++count; });
  EXPECT_EQ(count, 1);
}
