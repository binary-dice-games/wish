// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"

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

  EXPECT_EQ(node.class_key(), node.as<key_t>(dynamic::CLASS));
  EXPECT_EQ(node.class_key(), "Button"_key);
}

TEST_F(UiElementTest, ClassKeyStableAcrossRepeatedCalls) {
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"hi"})");
  ui_element& node = *map[""];

  key_t first = node.class_key();
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
