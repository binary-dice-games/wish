// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <ui/ui_importer.hpp>
#include "src/bison/bison_object.hpp"

#include <vector>

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

// ── Float arrays (plot data) ─────────────────────────────────────────────────

TEST_F(UiImporterTest, JsonFloatArrayReachesAFloatArrayField) {
  constexpr auto desc = R"({ "type": "PlotLine", "xs": [0.5, 1.5, 2], "ys": [1, 2, 3] })";

  auto result = bdg::wish::import_json(desc);
  ASSERT_TRUE(result.count(""));
  const auto& xs = result[""]->findField("xs"_key)->as<std::vector<float>>();
  ASSERT_EQ(xs.size(), 3u);
  EXPECT_FLOAT_EQ(xs[0], 0.5f);
  EXPECT_FLOAT_EQ(xs[2], 2.0f);
  // An all-integer literal array is widened to the field's float[] type.
  const auto& ys = result[""]->findField("ys"_key)->as<std::vector<float>>();
  ASSERT_EQ(ys.size(), 3u);
  EXPECT_FLOAT_EQ(ys[1], 2.0f);
}

TEST_F(UiImporterTest, YamlFloatArrayReachesAFloatArrayField) {
  auto result = bdg::wish::import_yaml("type: PlotLine\nxs: [0.25, 1]\nys: [3, 4]\n");
  ASSERT_TRUE(result.count(""));
  const auto& xs = result[""]->findField("xs"_key)->as<std::vector<float>>();
  ASSERT_EQ(xs.size(), 2u);
  EXPECT_FLOAT_EQ(xs[0], 0.25f);
  EXPECT_EQ(result[""]->findField("ys"_key)->as<std::vector<float>>().size(), 2u);
}

// ── Plot styling fields ──────────────────────────────────────────────────────

TEST_F(UiImporterTest, PlotSeriesStyleFieldsHaveAutomaticDefaults) {
  auto result = bdg::wish::import_json(R"({ "type": "PlotLine" })");
  auto& line = result[""];
  EXPECT_EQ(line->findField("color"_key)->as<std::string>(), "");
  EXPECT_EQ(line->findField("fill_color"_key)->as<std::string>(), "");
  EXPECT_FLOAT_EQ(line->findField("line_weight"_key)->as<float>(), 1.0f);
  EXPECT_FLOAT_EQ(line->findField("fill_alpha"_key)->as<float>(), -1.0f);
  EXPECT_EQ(line->findField("marker"_key)->as<int32_t>(), -3); // "Default"
  EXPECT_FLOAT_EQ(line->findField("marker_size"_key)->as<float>(), 0.0f);
  EXPECT_EQ(bdg::wish::import_json(R"({ "type": "Plot" })")[""]->findField("colormap"_key)->as<int32_t>(), -1);
}

TEST_F(UiImporterTest, PlotStyleFieldsAcceptNamesAndValues) {
  constexpr auto desc = R"({
    "type": "Plot", "colormap": "Viridis",
    "children": [
      { "type": "PlotBars", "color": "#C0392B", "fill_alpha": 0.5, "line_weight": 2, "marker": "Circle" }
    ]
  })";
  auto result = bdg::wish::import_json(desc);
  EXPECT_EQ(result[""]->findField("colormap"_key)->as<int32_t>(), 4);
  auto bars = result[""]->findField("children"_key)->as<dynamic_ptr>()->at(0).as<dynamic_ptr>();
  EXPECT_EQ(bars->findField("color"_key)->as<std::string>(), "#C0392B");
  EXPECT_FLOAT_EQ(bars->findField("fill_alpha"_key)->as<float>(), 0.5f);
  EXPECT_FLOAT_EQ(bars->findField("line_weight"_key)->as<float>(), 2.0f);
  EXPECT_EQ(bars->findField("marker"_key)->as<int32_t>(), 0);

  // Every series class inherits them, 3D included.
  for (const char* type : {"PlotScatter", "PlotShaded", "PlotHistogram", "PlotInfLines", "Plot3DLine",
           "Plot3DSurface", "Plot3DMesh"}) {
    auto one = bdg::wish::import_json(std::string(R"({ "type": ")") + type + R"(", "color": "#112233" })");
    EXPECT_EQ(one[""]->findField("color"_key)->as<std::string>(), "#112233") << type;
  }
  EXPECT_EQ(bdg::wish::import_json(R"({ "type": "Plot3D", "colormap": "Jet" })")[""]
                ->findField("colormap"_key)
                ->as<int32_t>(),
      9);
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

// ── DockLayout / DockSplit / DockArea ─────────────────────────────────────────

TEST_F(UiImporterTest, DockLayoutFamilyResolvesThroughImporter) {
  EXPECT_EQ(bdg::wish::import_json(R"({"type":"DockLayout"})")[""]->class_key(), "DockLayout"_key);
  EXPECT_EQ(bdg::wish::import_json(R"({"type":"DockSplit"})")[""]->class_key(), "DockSplit"_key);
  EXPECT_EQ(bdg::wish::import_json(R"({"type":"DockArea"})")[""]->class_key(), "DockArea"_key);
}

TEST_F(UiImporterTest, DockLayoutDescriptorRoundTrips) {
  constexpr auto desc = R"({
    "type": "DockLayout", "version": 3,
    "children": [
      { "type": "DockSplit", "dir": "left", "ratio": 0.62, "children": [
        { "type": "DockArea", "windows": "containers\nimages\nstats", "focused": "containers" },
        { "type": "DockArea", "windows": "logs" }
      ] }
    ]
  })";

  auto result = bdg::wish::import_json(desc);
  auto& root = result[""];
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->class_key(), "DockLayout"_key);
  EXPECT_EQ(root->findField("version"_key)->get_as<int32_t>(), 3);

  // Single DockSplit child.
  const bdg::wish::ui_element* split = nullptr;
  root->for_each_child_ordered([&](bdg::bison::key_t, bdg::wish::ui_element& c) { split = &c; });
  ASSERT_NE(split, nullptr);
  EXPECT_EQ(split->class_key(), "DockSplit"_key);
  EXPECT_EQ(split->findField("dir"_key)->as<std::string>(), "left");
  EXPECT_FLOAT_EQ(split->findField("ratio"_key)->get_as<float>(), 0.62f);

  // Two ordered DockArea children; first keeps the newline-joined window list.
  std::vector<bdg::wish::ui_element*> areas;
  split->for_each_child_ordered([&](bdg::bison::key_t, bdg::wish::ui_element& c) { areas.push_back(&c); });
  ASSERT_EQ(areas.size(), 2u);
  EXPECT_EQ(areas[0]->class_key(), "DockArea"_key);
  EXPECT_EQ(areas[0]->findField("windows"_key)->as<std::string>(), "containers\nimages\nstats");
  EXPECT_EQ(areas[0]->findField("focused"_key)->as<std::string>(), "containers");
  EXPECT_EQ(areas[1]->findField("windows"_key)->as<std::string>(), "logs");
}

// Guards against "improving" DockArea.windows into a JSON array: the client
// descriptor importer silently drops array/object-valued scalar fields, so a
// template would lose the window list entirely.
TEST_F(UiImporterTest, DockAreaWindowsStaysAScalarString) {
  auto result = bdg::wish::import_json(
      R"({ "type": "DockArea", "windows": "a\nb\nc" })");
  auto& area = result[""];
  ASSERT_NE(area, nullptr);
  const auto* f = area->findField("windows"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "a\nb\nc");
}
