// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <registry.hpp>
#include <ui_importer.hpp>
#include "src/bison/bison_object.hpp"

using namespace bdg::bison;

class UiImporterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

// ── Simple Window ─────────────────────────────────────────────────────────────

TEST_F(UiImporterTest, JsonWindowNoChildren) {
  constexpr auto desc = R"({
    "type": "Window",
    "title": "Hello",
    "width": 400,
    "height": 300
  })";

  auto result = bdg::wish::import_json(desc);

  ASSERT_TRUE(result.count(""));
  auto& win = result[""];
  ASSERT_NE(win, nullptr);
  EXPECT_EQ(win->findField(dynamic::CLASS)->as<bdg::bison::key_t>(), "Window"_key);
  EXPECT_EQ(win->findField("title"_key)->as<std::string>(), "Hello");
  EXPECT_EQ(win->findField("width"_key)->as<int32_t>(), 400);
  EXPECT_EQ(win->findField("height"_key)->as<int32_t>(), 300);
}

// ── Named child ───────────────────────────────────────────────────────────────

TEST_F(UiImporterTest, JsonNamedChildInMap) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": {
      "ok": { "type": "Button", "label": "OK" }
    }
  })";

  auto result = bdg::wish::import_json(desc);

  ASSERT_TRUE(result.count("ok"));
  auto& btn = result["ok"];
  ASSERT_NE(btn, nullptr);
  EXPECT_EQ(btn->findField(dynamic::CLASS)->as<bdg::bison::key_t>(), "Button"_key);
  EXPECT_EQ(btn->findField("label"_key)->as<std::string>(), "OK");
}

TEST_F(UiImporterTest, JsonNamedChildAccessibleFromParentChildren) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": {
      "ok": { "type": "Button", "label": "OK" }
    }
  })";

  auto result = bdg::wish::import_json(desc);
  auto& win = result[""];

  auto children = win->findField("children"_key)->as<dynamic_ptr>();
  ASSERT_NE(children, nullptr);
  auto& btn_field = (*children)["ok"_key];
  EXPECT_TRUE(btn_field.is<dynamic_ptr>());
}

// ── Deep hierarchy ────────────────────────────────────────────────────────────

TEST_F(UiImporterTest, JsonDeepHierarchyAllNamedNodesInMap) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": {
      "body": {
        "type": "VerticalLayout",
        "children": {
          "row1": {
            "type": "HorizontalLayout",
            "children": {
              "lbl1": { "type": "Label", "text": "A" },
              "btn1": { "type": "Button", "label": "B" }
            }
          },
          "row2": {
            "type": "HorizontalLayout",
            "children": {
              "lbl2": { "type": "Label", "text": "C" },
              "btn2": { "type": "Button", "label": "D" }
            }
          }
        }
      }
    }
  })";

  auto result = bdg::wish::import_json(desc);

  EXPECT_EQ(result.size(), 8u);
  EXPECT_TRUE(result.count(""));
  EXPECT_TRUE(result.count("body"));
  EXPECT_TRUE(result.count("body.row1"));
  EXPECT_TRUE(result.count("body.row1.lbl1"));
  EXPECT_TRUE(result.count("body.row1.btn1"));
  EXPECT_TRUE(result.count("body.row2"));
  EXPECT_TRUE(result.count("body.row2.lbl2"));
  EXPECT_TRUE(result.count("body.row2.btn2"));

  EXPECT_EQ(result["body"]->findField(dynamic::CLASS)->as<bdg::bison::key_t>(), "VerticalLayout"_key);
  EXPECT_EQ(result["body.row1"]->findField(dynamic::CLASS)->as<bdg::bison::key_t>(), "HorizontalLayout"_key);
  EXPECT_EQ(result["body.row1.lbl1"]->findField("text"_key)->as<std::string>(), "A");
}

// ── visible field ─────────────────────────────────────────────────────────────

TEST_F(UiImporterTest, JsonVisibleFalseIsApplied) {
  constexpr auto desc = R"({ "type": "Label", "text": "Hi", "visible": false })";

  auto result = bdg::wish::import_json(desc);
  ASSERT_TRUE(result.count(""));
  EXPECT_FALSE(result[""]->findField("visible"_key)->as<bool>());
}

// ── Indexed children ──────────────────────────────────────────────────────────

TEST_F(UiImporterTest, JsonIndexedChildrenAccessibleByIndex) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": [
      { "type": "Label",  "text": "First" },
      { "type": "Button", "label": "Second" }
    ]
  })";

  auto result = bdg::wish::import_json(desc);
  ASSERT_TRUE(result.count(""));

  // Indexed children are NOT in the name_map.
  EXPECT_EQ(result.size(), 1u);

  auto children = result[""]->findField("children"_key)->as<dynamic_ptr>();
  ASSERT_NE(children, nullptr);

  auto& c0 = children->at(0);
  auto& c1 = children->at(1);
  EXPECT_TRUE(c0.is<dynamic_ptr>());
  EXPECT_TRUE(c1.is<dynamic_ptr>());

  auto lbl = c0.as<dynamic_ptr>();
  EXPECT_EQ(lbl->findField(dynamic::CLASS)->as<bdg::bison::key_t>(), "Label"_key);
  EXPECT_EQ(lbl->findField("text"_key)->as<std::string>(), "First");
}

// ── Numeric float coercion ────────────────────────────────────────────────────

TEST_F(UiImporterTest, JsonIntegerCoercedToFloatForSpacing) {
  constexpr auto desc = R"({ "type": "VerticalLayout", "spacing": 8 })";

  auto result = bdg::wish::import_json(desc);
  ASSERT_TRUE(result.count(""));
  EXPECT_FLOAT_EQ(result[""]->findField("spacing"_key)->as<float>(), 8.0f);
}

// ── Error cases ───────────────────────────────────────────────────────────────

TEST_F(UiImporterTest, JsonUnknownTypeThrows) {
  EXPECT_THROW(bdg::wish::import_json(R"({ "type": "DoesNotExist" })"), std::runtime_error);
}

TEST_F(UiImporterTest, JsonInvalidJsonThrows) {
  EXPECT_THROW(bdg::wish::import_json("{ this is not valid json }"), std::runtime_error);
}

TEST_F(UiImporterTest, JsonMissingTypeThrows) {
  EXPECT_THROW(bdg::wish::import_json(R"({ "title": "Hello" })"), std::runtime_error);
}

// ── YAML round-trip ───────────────────────────────────────────────────────────

TEST_F(UiImporterTest, YamlRoundTripMatchesJson) {
  constexpr auto json_desc = R"({
    "type": "Window",
    "title": "Hello",
    "width": 400,
    "children": {
      "ok": { "type": "Button", "label": "OK" }
    }
  })";

  constexpr auto yaml_desc = R"(
type: Window
title: Hello
width: 400
children:
  ok:
    type: Button
    label: OK
)";

  auto json_result = bdg::wish::import_json(json_desc);
  auto yaml_result = bdg::wish::import_yaml(yaml_desc);

  ASSERT_EQ(json_result.size(), yaml_result.size());

  ASSERT_TRUE(yaml_result.count(""));
  ASSERT_TRUE(yaml_result.count("ok"));

  auto& win = yaml_result[""];
  EXPECT_EQ(win->findField(dynamic::CLASS)->as<bdg::bison::key_t>(), "Window"_key);
  EXPECT_EQ(win->findField("title"_key)->as<std::string>(), "Hello");
  EXPECT_EQ(win->findField("width"_key)->as<int32_t>(), 400);

  auto& btn = yaml_result["ok"];
  EXPECT_EQ(btn->findField(dynamic::CLASS)->as<bdg::bison::key_t>(), "Button"_key);
  EXPECT_EQ(btn->findField("label"_key)->as<std::string>(), "OK");
}

// ── Reserved-field collision ──────────────────────────────────────────────────

// A widget field literally named "name" (as opposed to the internal
// "__name__"/"__path__" bookkeeping fields build_ui_node stamps on mapped
// nodes) must round-trip untouched and not be confused with the child's own
// dot-path identity.
TEST_F(UiImporterTest, NameFieldDoesNotCollideWithPathBookkeeping) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": {
      "ok": { "type": "Button", "label": "OK", "name": "literal-name-value" }
    }
  })";

  auto result = bdg::wish::import_json(desc);

  ASSERT_TRUE(result.count("ok"));
  auto& btn = result["ok"];
  ASSERT_NE(btn, nullptr);
  EXPECT_EQ(btn->findField("name"_key)->as<std::string>(), "literal-name-value");
  EXPECT_EQ(btn->findField("label"_key)->as<std::string>(), "OK");
}

TEST_F(UiImporterTest, YamlVisibleFalseIsApplied) {
  constexpr auto yaml_desc = R"(
type: Label
text: Hi
visible: false
)";

  auto result = bdg::wish::import_yaml(yaml_desc);
  ASSERT_TRUE(result.count(""));
  EXPECT_FALSE(result[""]->findField("visible"_key)->as<bool>());
}
