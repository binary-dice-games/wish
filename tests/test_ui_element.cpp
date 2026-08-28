// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"

#include <string>
#include <vector>

using namespace bdg::bison;
using bdg::wish::ui_element;

class UiElementTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

// ── class_key() ──────────────────────────────────────────────────────────────

TEST_F(UiElementTest, ClassKeyMatchesRawClassField) {
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  ui_element& node = *map[""];

  EXPECT_EQ(node.class_key(), node.as<bdg::bison::key_t>(dynamic::CLASS));
  EXPECT_EQ(node.class_key(), "Button"_key);
}

TEST_F(UiElementTest, ClassKeyStableAcrossRepeatedCalls) {
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  ui_element& node = *map[""];

  bdg::bison::key_t first = node.class_key();
  for (int i = 0; i < 5; ++i)
    EXPECT_EQ(node.class_key(), first);
}

TEST_F(UiElementTest, ClassKeyCorrectOnImportedChild) {
  auto map = bdg::wish::import_json(
      R"({"type":"VerticalLayout","children":{"a":{"type":"Label","text":"x"}}})");
  ui_element& child = *map["a"];

  EXPECT_EQ(child.class_key(), "Label"_key);
}

// ── visible() ─────────────────────────────────────────────────────────────────

TEST_F(UiElementTest, VisibleDefaultsTrueWhenFieldNeverSet) {
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  ui_element& node = *map[""];

  EXPECT_TRUE(node.visible());
}

TEST_F(UiElementTest, VisibleReflectsWriteThroughUiElementOperator) {
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  ui_element& node = *map[""];

  ASSERT_TRUE(node.visible());
  node["visible"_key] = false;
  EXPECT_FALSE(node.visible());
}

TEST_F(UiElementTest, VisibleReflectsWriteThroughBaseDynamicReference) {
  // Proves the pointer-cache approach survives writes made through a plain
  // bison::dynamic& reference to the same object -- not just through
  // ui_element-specific call sites -- since ui_element cannot intercept
  // writes made via dynamic's non-virtual accessors.
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  ui_element& node = *map[""];
  dynamic& base = node;

  ASSERT_TRUE(node.visible());
  base["visible"_key] = false;
  EXPECT_FALSE(node.visible());
}

TEST_F(UiElementTest, VisibleReflectsSecondWriteNotJustFirst) {
  // Confirms the cache freezes the field *pointer*, not the value: a second
  // write after the pointer is already cached must still be observed.
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  ui_element& node = *map[""];

  node["visible"_key] = false;
  ASSERT_FALSE(node.visible());
  node["visible"_key] = true;
  EXPECT_TRUE(node.visible());
}

// ── Round 2: label(), width()/width_i(), value_bool()/value_float(), selected() ──

TEST_F(UiElementTest, LabelDefaultsToArgumentWhenFieldNeverSet) {
  // Constructed directly rather than through the registered "Button"
  // prototype (which pre-populates "label" with "" -- a set-but-empty
  // field, which correctly ignores the fallback), so the field is
  // genuinely absent.
  bdg::wish::ui_button node{dynamic{}};

  EXPECT_EQ(node.label("fallback"), "fallback");
}

TEST_F(UiElementTest, LabelReflectsWriteThroughUiElementOperator) {
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  auto& node = static_cast<bdg::wish::ui_button&>(*map[""]);

  ASSERT_EQ(node.label(), "hi");
  node["label"_key] = std::string("bye");
  EXPECT_EQ(node.label(), "bye");
}

TEST_F(UiElementTest, LabelReflectsWriteThroughBaseDynamicReference) {
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  auto& node = static_cast<bdg::wish::ui_button&>(*map[""]);
  dynamic& base = node;

  ASSERT_EQ(node.label(), "hi");
  base["label"_key] = std::string("bye");
  EXPECT_EQ(node.label(), "bye");
}

TEST_F(UiElementTest, LabelReflectsSecondWriteNotJustFirst) {
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  auto& node = static_cast<bdg::wish::ui_button&>(*map[""]);

  node["label"_key] = std::string("first");
  ASSERT_EQ(node.label(), "first");
  node["label"_key] = std::string("second");
  EXPECT_EQ(node.label(), "second");
}

TEST_F(UiElementTest, WidthAndWidthIShareOneCachedFieldButReadTheirOwnType) {
  // width()/width_i() share one mutable field* cache member -- the cache
  // pins the field's location, not its stored type, so each accessor still
  // reads whichever type that particular node's "width" field actually
  // holds.
  auto float_map = bdg::wish::import_json(R"({"type":"Button","width":12.5})");
  ui_element& float_node = *float_map[""];
  EXPECT_FLOAT_EQ(float_node.width(), 12.5f);
  EXPECT_FLOAT_EQ(float_node.width(), 12.5f);  // second call hits the cache

  auto int_map = bdg::wish::import_json(R"({"type":"Window"})");
  ui_element& int_node = *int_map[""];
  int_node["width"_key] = int32_t(200);
  EXPECT_EQ(int_node.width_i(), 200);
  EXPECT_EQ(int_node.width_i(), 200);  // second call hits the cache
}

TEST_F(UiElementTest, WidthDefaultsToArgumentWhenFieldNeverSet) {
  auto map = bdg::wish::import_json(R"({"type":"Button"})");
  ui_element& node = *map[""];

  EXPECT_FLOAT_EQ(node.width(-1.0f), -1.0f);
  EXPECT_EQ(node.width_i(7), 7);
}

TEST_F(UiElementTest, ValueBoolAndValueFloatShareOneCachedFieldButReadTheirOwnType) {
  auto bool_map = bdg::wish::import_json(R"({"type":"Checkbox"})");
  auto& bool_node = static_cast<bdg::wish::ui_checkbox&>(*bool_map[""]);
  bool_node["value"_key] = true;
  EXPECT_TRUE(bool_node.value_bool());
  EXPECT_TRUE(bool_node.value_bool());  // second call hits the cache

  auto float_map = bdg::wish::import_json(R"({"type":"SliderFloat"})");
  auto& float_node = static_cast<bdg::wish::ui_slider_float&>(*float_map[""]);
  float_node["value"_key] = 3.5f;
  EXPECT_FLOAT_EQ(float_node.value_float(), 3.5f);
  EXPECT_FLOAT_EQ(float_node.value_float(), 3.5f);  // second call hits the cache
}

TEST_F(UiElementTest, WidthCoercesIntFieldToFloatLikeGetAsDid) {
  // Regression test: a JSON integer literal like "width": -1 (e.g.
  // file_dialog.cpp's stretch-to-fill Table) is parsed as an int32_t
  // field. The old `node.get_as<float>("width"_key, 0.0f)` call coerced
  // that to -1.0f via field::get_as<T>()'s cross-type conversion.
  // cached_field_or() must do the same via field::get_as<T>(), not the
  // stricter field::as<T>()/is<T>() pair -- using the latter silently
  // fell back to the default (0.0f) for an int32_t-stored field, which
  // misclassified a stretch-fill Table as "auto" in arrange_vertical_
  // layout() and reintroduced the unbounded per-frame growth bug this
  // code's own comments describe.
  auto map = bdg::wish::import_json(R"({"type":"Button","width":-1})");
  ui_element& node = *map[""];

  ASSERT_TRUE(node.findField("width"_key)->is<int32_t>());
  EXPECT_FLOAT_EQ(node.width(0.0f), -1.0f);
  EXPECT_FLOAT_EQ(node.width(0.0f), -1.0f);  // second call hits the cache
}

TEST_F(UiElementTest, SelectedDefaultsFalseAndReflectsWrites) {
  auto map = bdg::wish::import_json(R"({"type":"Selectable","label":"row"})");
  auto& node = static_cast<bdg::wish::ui_selectable&>(*map[""]);

  EXPECT_FALSE(node.selected());
  node["selected"_key] = true;
  EXPECT_TRUE(node.selected());
  node["selected"_key] = false;
  EXPECT_FALSE(node.selected());
}

// ── drag_type() / drag_payload() / drop_type() ─────────────────────────────

TEST_F(UiElementTest, DragDropAccessorsDefaultEmptyAndReflectWrites) {
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  ui_element& node = *map[""];

  EXPECT_EQ(node.drag_type(), "");
  EXPECT_EQ(node.drag_payload(), "");
  EXPECT_EQ(node.drop_type(), "");

  node["drag_type"_key] = std::string("file");
  node["drag_payload"_key] = std::string("/tmp/a");
  node["drop_type"_key] = std::string("file");
  EXPECT_EQ(node.drag_type(), "file");
  EXPECT_EQ(node.drag_payload(), "/tmp/a");
  EXPECT_EQ(node.drop_type(), "file");

  // Second write after the field pointer is cached must still be observed.
  node["drag_payload"_key] = std::string("/tmp/b");
  EXPECT_EQ(node.drag_payload(), "/tmp/b");
}

// ── set_self_rect() / self_rect() ──────────────────────────────────────────

TEST_F(UiElementTest, SelfRectReturnsFalseUntilStamped) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout"})");
  ui_element& node = *map[""];

  bdg::wish::vec2f pos{1.0f, 1.0f};
  bdg::wish::vec2f size{2.0f, 2.0f};
  EXPECT_FALSE(node.self_rect(pos, size));
  // Outputs left untouched on a miss.
  EXPECT_FLOAT_EQ(pos.x, 1.0f);
  EXPECT_FLOAT_EQ(size.x, 2.0f);
}

TEST_F(UiElementTest, SelfRectRoundTripsThroughStampedFields) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout"})");
  ui_element& node = *map[""];

  node.set_self_rect({10.0f, 20.0f}, {300.0f, 400.0f});

  bdg::wish::vec2f pos;
  bdg::wish::vec2f size;
  ASSERT_TRUE(node.self_rect(pos, size));
  EXPECT_FLOAT_EQ(pos.x, 10.0f);
  EXPECT_FLOAT_EQ(pos.y, 20.0f);
  EXPECT_FLOAT_EQ(size.x, 300.0f);
  EXPECT_FLOAT_EQ(size.y, 400.0f);

  // A later frame's re-stamp is observed through the cached field pointers.
  node.set_self_rect({11.0f, 21.0f}, {301.0f, 401.0f});
  ASSERT_TRUE(node.self_rect(pos, size));
  EXPECT_FLOAT_EQ(pos.x, 11.0f);
  EXPECT_FLOAT_EQ(size.y, 401.0f);
}

TEST_F(UiElementTest, SelfRectVisibleThroughRawFields) {
  // render_node() previously read these back via findField() directly; the
  // stamped values must be visible under the literal "__wish_win_rect_*__"
  // keys for any external/observability consumer.
  auto map = bdg::wish::import_json(R"({"type":"Window","title":"w"})");
  ui_element& node = *map[""];

  node.set_self_rect({5.0f, 6.0f}, {7.0f, 8.0f});
  EXPECT_FLOAT_EQ(node.as<float>("__wish_win_rect_x__"_key), 5.0f);
  EXPECT_FLOAT_EQ(node.as<float>("__wish_win_rect_h__"_key), 8.0f);
}

// ── for_each_child_ordered() / clone_ptr() ──────────────────────────────────
//
// Regression coverage for a bug where cloning a tree (as ui_template.cpp's
// prototype instantiation does for every template-instantiated UI) left the
// clone's for_each_child_ordered() cache unbuilt, silently falling back to
// hash-sorted map order instead of declaration order -- see ui_element::
// clone_ptr()'s doc comment.

static std::vector<std::string> collect_labels_in_order(const ui_element& node) {
  std::vector<std::string> labels;
  node.for_each_child_ordered(
      [&](bdg::bison::key_t, ui_element& child) { labels.push_back(child.as<std::string>("label"_key)); });
  return labels;
}

TEST_F(UiElementTest, ForEachChildOrderedMatchesDeclarationOrderBeforeClone) {
  // Named children whose keys hash in a different order than they're
  // declared -- if for_each_child_ordered() ever fell back to hash-sorted
  // map order, this would catch it even before any clone is involved.
  auto map = bdg::wish::import_json(R"({
    "type": "VerticalLayout",
    "children": {
      "zzz_child": {"type": "Button", "label": "First"},
      "aaa_child": {"type": "Button", "label": "Second"},
      "mmm_child": {"type": "Button", "label": "Third"}
    }
  })");
  ui_element& root = *map[""];

  EXPECT_EQ(collect_labels_in_order(root), (std::vector<std::string>{"First", "Second", "Third"}));
}

TEST_F(UiElementTest, ForEachChildOrderedMatchesDeclarationOrderAfterClone) {
  auto map = bdg::wish::import_json(R"({
    "type": "VerticalLayout",
    "children": {
      "zzz_child": {"type": "Button", "label": "First"},
      "aaa_child": {"type": "Button", "label": "Second"},
      "mmm_child": {"type": "Button", "label": "Third"}
    }
  })");
  ui_element& root = *map[""];

  auto cloned = std::static_pointer_cast<ui_element>(std::shared_ptr<dynamic>(root.clone_ptr()));

  EXPECT_EQ(collect_labels_in_order(*cloned), (std::vector<std::string>{"First", "Second", "Third"}));
}

TEST_F(UiElementTest, ForEachChildOrderedMatchesDeclarationOrderAfterCloneOfTypedLeaf) {
  // Same as above but for a leaf that goes through cloneable_ui_element<T>
  // (e.g. TabBar/TabItem) rather than plain ui_element's own clone_ptr() --
  // both override clone_ptr() independently and both need the fix.
  auto map = bdg::wish::import_json(R"({
    "type": "TabBar",
    "children": {
      "zzz_tab": {"type": "TabItem", "label": "First"},
      "aaa_tab": {"type": "TabItem", "label": "Second"},
      "mmm_tab": {"type": "TabItem", "label": "Third"}
    }
  })");
  ui_element& root = *map[""];

  auto cloned = std::static_pointer_cast<ui_element>(std::shared_ptr<dynamic>(root.clone_ptr()));

  EXPECT_EQ(collect_labels_in_order(*cloned), (std::vector<std::string>{"First", "Second", "Third"}));
}
