// MIT License © 2026 Binary Dice Games
/// @file test_nymph_bind.cpp
/// @brief nymph's format validation and `$column` data binding, against the
///        real wish class registry.
#include <gtest/gtest.h>

#include <context/context.hpp>
#include <server/registry.hpp>
#include <ui/ui_importer.hpp>
#include <ui/ui_schema_help.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include "modules/bdg/dev/nymph/server/nymph_bind.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace bdg::bison;
namespace nymph = bdg::wish::nymph;

namespace {

// The example from DESIGN.md "3. Source Format". Line numbers matter below.
const std::string kExample = "Monthly revenue against cost, 2025.\n" // 1
                             "--\n"                                  // 2
                             "image:\n"                              // 3
                             "  width: 800\n"                        // 4
                             "  height: 450\n"                       // 5
                             "root:\n"                               // 6
                             "  type: Plot\n"                        // 7
                             "  title: Revenue vs cost\n"            // 8
                             "  x_label: month\n"                    // 9
                             "  y_label: kUSD\n"                     // 10
                             "  children:\n"                         // 11
                             "    - type: PlotLine\n"                // 12
                             "      label: revenue\n"                // 13
                             "      xs: $month\n"                    // 14
                             "      ys: $revenue\n"                  // 15
                             "    - type: PlotBars\n"                // 16
                             "      label: cost\n"                   // 17
                             "      xs: $month\n"                    // 18
                             "      ys: $cost\n"                     // 19
                             "--\n"                                  // 20
                             "month, revenue, cost\n"                // 21
                             "1, 12, 9\n"                            // 22
                             "2, 15, 9.5\n";                         // 23

std::string source(const std::string& format, const std::string& data = "a,b,name\n1,2,x\n3,4,y\n") {
  return "desc\n--\n" + format + "--\n" + data;
}

nymph::figure bind_text(const std::string& text) {
  return nymph::bind(nymph::parse_document(text));
}

/// Line of the error binding @p text raises; 0 if it binds.
int error_line(const std::string& text, std::string* message = nullptr) {
  try {
    bind_text(text);
  } catch (const nymph::error& e) {
    if (message)
      *message = e.message();
    return e.line();
  }
  return 0;
}

/// Child @p index of a bound element.
const dynamic& child(const dynamic& element, size_t index) {
  const auto& children = element.findField("children"_key)->as<dynamic_ptr>();
  return *children->at(index).as<dynamic_ptr>();
}

class NymphBindTest : public ::testing::Test {
 protected:
  void SetUp() override { bdg::wish::register_all(); }
};

} // namespace

// ── The happy path ───────────────────────────────────────────────────────────

TEST_F(NymphBindTest, ExampleBinds) {
  auto fig = bind_text(kExample);
  EXPECT_EQ(fig.options.width, 800);
  EXPECT_EQ(fig.options.height, 450);
  EXPECT_EQ(fig.options.scale, 1);
  EXPECT_EQ(fig.options.theme, "light");
  EXPECT_EQ(fig.data.row_count(), 2u);

  EXPECT_EQ(fig.root.findField("__type__"_key)->as<bdg::bison::key_t>(), "Plot"_key);
  EXPECT_EQ(fig.root.findField("title"_key)->as<std::string>(), "Revenue vs cost");

  const dynamic& line = child(fig.root, 0);
  EXPECT_EQ(line.findField("__type__"_key)->as<bdg::bison::key_t>(), "PlotLine"_key);
  const auto& xs = line.findField("xs"_key)->as<std::vector<float>>();
  const auto& ys = line.findField("ys"_key)->as<std::vector<float>>();
  ASSERT_EQ(xs.size(), 2u);
  EXPECT_FLOAT_EQ(xs[0], 1.0f);
  EXPECT_FLOAT_EQ(xs[1], 2.0f);
  EXPECT_FLOAT_EQ(ys[1], 15.0f);

  const dynamic& bars = child(fig.root, 1);
  EXPECT_FLOAT_EQ(bars.findField("ys"_key)->as<std::vector<float>>()[1], 9.5f);
  // Children keep their declaration order.
  EXPECT_EQ(line.findField("order"_key)->as<int32_t>(), 0);
  EXPECT_EQ(bars.findField("order"_key)->as<int32_t>(), 1);
}

TEST_F(NymphBindTest, BoundDescriptorBuildsRealElements) {
  auto fig = bind_text(kExample);
  bdg::wish::name_map names;
  auto root = bdg::wish::build_ui_node(fig.root, "", true, names);
  ASSERT_TRUE(root);
  EXPECT_EQ(root->findField("x_label"_key)->as<std::string>(), "month");
  const auto& children = root->findField("children"_key)->as<dynamic_ptr>();
  const auto& line = children->at(0).as<dynamic_ptr>();
  EXPECT_EQ(line->findField("ys"_key)->as<std::vector<float>>().size(), 2u);
}

TEST_F(NymphBindTest, LonePlotFillsTheImageUnlessItSaysOtherwise) {
  auto fig = bind_text(kExample);
  EXPECT_FLOAT_EQ(fig.root.findField("height"_key)->as<float>(), -1.0f); // "all that is left"
  auto fixed = bind_text(source("root:\n  type: Plot\n  height: 120\n"));
  EXPECT_FLOAT_EQ(fixed.root.findField("height"_key)->as<float>(), 120.0f);
  auto layout = bind_text(source("root:\n  type: VerticalLayout\n"));
  EXPECT_EQ(layout.root.findField("height"_key), nullptr);
}

TEST_F(NymphBindTest, LiteralArraysNeedNoData) {
  auto fig = bind_text(source("root:\n  type: Plot\n  children:\n    - type: PlotLine\n      xs: [0, 1, 2]\n"
                         "      ys: [1.5, 2, 2.5]\n",
      ""));
  const dynamic& line = child(fig.root, 0);
  EXPECT_EQ(line.findField("xs"_key)->as<std::vector<float>>().size(), 3u);
  EXPECT_FLOAT_EQ(line.findField("ys"_key)->as<std::vector<float>>()[0], 1.5f);
}

TEST_F(NymphBindTest, ReferencesByPositionAndQuotedNames) {
  auto fig = bind_text(source("root:\n  type: Plot\n  children:\n    - type: PlotLine\n      xs: $1\n"
                         "      ys: \"$unit price\"\n",
      "t,unit price\n1,10\n2,20\n"));
  const dynamic& line = child(fig.root, 0);
  EXPECT_FLOAT_EQ(line.findField("xs"_key)->as<std::vector<float>>()[1], 2.0f);
  EXPECT_FLOAT_EQ(line.findField("ys"_key)->as<std::vector<float>>()[1], 20.0f);
}

TEST_F(NymphBindTest, ListOfReferencesInterleavesByRow) {
  auto fig = bind_text(source("root:\n  type: Plot\n  children:\n    - type: PlotHeatmap\n      rows: 2\n"
                         "      cols: 2\n      values: [$a, $b]\n"));
  const auto& values = child(fig.root, 0).findField("values"_key)->as<std::vector<float>>();
  ASSERT_EQ(values.size(), 4u);
  EXPECT_FLOAT_EQ(values[0], 1.0f); // row 0: a, b
  EXPECT_FLOAT_EQ(values[1], 2.0f);
  EXPECT_FLOAT_EQ(values[2], 3.0f); // row 1: a, b
  EXPECT_FLOAT_EQ(values[3], 4.0f);
}

TEST_F(NymphBindTest, ReferenceOnATextFieldJoinsWithNewlines) {
  auto fig = bind_text(source("root:\n  type: Plot\n  children:\n    - type: PlotPieChart\n      labels: $name\n"
                         "      values: $a\n"));
  EXPECT_EQ(child(fig.root, 0).findField("labels"_key)->as<std::string>(), "x\ny");
}

TEST_F(NymphBindTest, ReferenceOnAnIntegerListRounds) {
  auto fig = bind_text(source("root:\n  type: Plot3D\n  children:\n    - type: Plot3DMesh\n      xs: $a\n"
                         "      ys: $a\n      zs: $b\n      indices: $a\n"));
  const auto& indices = child(fig.root, 0).findField("indices"_key)->as<std::vector<int32_t>>();
  ASSERT_EQ(indices.size(), 2u);
  EXPECT_EQ(indices[1], 3);
}

TEST_F(NymphBindTest, EscapedDollarIsALiteral) {
  auto fig = bind_text(source("root:\n  type: Plot\n  title: $$5 per unit\n"));
  EXPECT_EQ(fig.root.findField("title"_key)->as<std::string>(), "$5 per unit");
}

TEST_F(NymphBindTest, NumberLookingTextStaysTextOnATextField) {
  auto fig = bind_text(source("root:\n  type: Plot\n  title: 2025\n"));
  EXPECT_EQ(fig.root.findField("title"_key)->as<std::string>(), "2025");
}

TEST_F(NymphBindTest, EmptyCellIsAGap) {
  auto fig = bind_text(source("root:\n  type: Plot\n  children:\n    - type: PlotLine\n      xs: $a\n      ys: $b\n",
      "a,b\n1,2\n2,\n3,4\n"));
  const auto& ys = child(fig.root, 0).findField("ys"_key)->as<std::vector<float>>();
  ASSERT_EQ(ys.size(), 3u);
  EXPECT_TRUE(std::isnan(ys[1]));
}

TEST_F(NymphBindTest, NamedChildrenAndNestedLayouts) {
  auto fig = bind_text(source("root:\n  type: VerticalLayout\n  children:\n    top:\n      type: Plot\n"
                         "      height: 200\n      children:\n        - type: PlotLine\n          ys: $a\n"
                         "    note:\n      type: Label\n      text: hello\n"));
  const auto& children = fig.root.findField("children"_key)->as<dynamic_ptr>();
  const auto& top = children->findField("top"_key)->as<dynamic_ptr>();
  EXPECT_EQ(top->findField("__name__"_key)->as<std::string>(), "top");
  EXPECT_EQ(children->findField("note"_key)->as<dynamic_ptr>()->findField("text"_key)->as<std::string>(), "hello");
}

TEST_F(NymphBindTest, NamedFlagValuesResolveThroughTheField) {
  // Plot.flags is an EnumFlags field: names are converted by the field itself.
  auto fig = bind_text(source("root:\n  type: Plot\n  flags: NoLegend\n"));
  EXPECT_NE(fig.root.findField("flags"_key)->as<int32_t>(), 0);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  flags: NotAFlag\n")), 5);
}

TEST_F(NymphBindTest, ImageOptions) {
  auto fig = bind_text(source("image: {width: 320, height: 200, scale: 2, theme: dark, padding: 0}\nroot:\n  type: Plot\n"));
  EXPECT_EQ(fig.options.width, 320);
  EXPECT_EQ(fig.options.height, 200);
  EXPECT_EQ(fig.options.scale, 2);
  EXPECT_EQ(fig.options.theme, "dark");
  EXPECT_EQ(fig.options.padding, 0);
}

// ── The allow list ───────────────────────────────────────────────────────────

TEST_F(NymphBindTest, EveryRegisteredPlotClassIsAllowed) {
  auto allowed = nymph::allowed_types();
  size_t plot_classes = 0;
  for (const auto& info : bdg::wish::enumerate_ui_element_classes()) {
    if (info.name.rfind("Plot", 0) != 0 || info.name == "PlotItem" || info.name == "Plot3DItem")
      continue;
    ++plot_classes;
    EXPECT_TRUE(std::binary_search(allowed.begin(), allowed.end(), info.name)) << info.name;
    // And each one binds in the place it belongs.
    const bool is3d = info.name.rfind("Plot3D", 0) == 0;
    std::string format;
    if (info.name == "Plot" || info.name == "Plot3D")
      format = "root:\n  type: " + info.name + "\n";
    else
      format = std::string("root:\n  type: ") + (is3d ? "Plot3D" : "Plot") + "\n  children:\n    - type: " +
               info.name + "\n";
    EXPECT_EQ(error_line(source(format)), 0) << info.name;
  }
  EXPECT_GE(plot_classes, 20u); // Plot + 15 series, Plot3D + 7
  for (const char* layout : {"VerticalLayout", "HorizontalLayout", "Spring", "Label", "Separator"})
    EXPECT_TRUE(std::binary_search(allowed.begin(), allowed.end(), std::string(layout))) << layout;
}

TEST_F(NymphBindTest, OtherElementTypesAreRejected) {
  for (const char* type : {"Image", "TextEditor", "Button", "Window", "InputText", "PlotItem", "NoSuchThing"})
    EXPECT_EQ(error_line(source(std::string("root:\n  type: ") + type + "\n")), 4) << type;
  // Also when nested.
  EXPECT_EQ(error_line(source("root:\n  type: VerticalLayout\n  children:\n    - type: Image\n      src: /etc/passwd\n")),
      6);
}

TEST_F(NymphBindTest, SeriesMustSitInTheRightKindOfPlot) {
  EXPECT_EQ(error_line(source("root:\n  type: PlotLine\n")), 4);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  children:\n    - type: Plot3DLine\n")), 6);
  EXPECT_EQ(error_line(source("root:\n  type: Plot3D\n  children:\n    - type: PlotLine\n")), 6);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  children:\n    - type: Label\n")), 6);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  children:\n    - type: Plot\n")), 6);
  EXPECT_EQ(error_line(source("root:\n  type: Label\n  children:\n    - type: Label\n")), 5);
}

// ── Errors carry the line in the whole source ────────────────────────────────

TEST_F(NymphBindTest, UnknownColumnNamesTheColumnsThatExist) {
  std::string text = kExample;
  text.replace(text.find("$revenue"), 8, "$revenu");
  std::string message;
  EXPECT_EQ(error_line(text, &message), 15);
  EXPECT_NE(message.find("revenu"), std::string::npos);
  EXPECT_NE(message.find("month, revenue, cost"), std::string::npos);
}

TEST_F(NymphBindTest, ErrorColumnPointsAtTheValue) {
  std::string text = kExample;
  text.replace(text.find("$revenue"), 8, "$revenu");
  try {
    bind_text(text);
    FAIL() << "expected an error";
  } catch (const nymph::error& e) {
    EXPECT_EQ(e.column(), 11); // "      ys: " is ten characters
  }
}

TEST_F(NymphBindTest, FormatErrors) {
  // format starts on line 3 of source()
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  title: $name\n  width: $a\n")), 6);          // ref on a float
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  children:\n    - type: PlotLine\n      ys: [1, $a]\n")), 7);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  children:\n    - type: PlotLine\n      ys: [1, x]\n")), 7);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  children:\n    - type: PlotLine\n      ys: 5\n")), 7);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  title: [1, 2]\n")), 5);                    // list on text
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  no_such_field: 1\n")), 5);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  height: tall\n")), 5);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  flags: 1.5\n")), 5);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  __wish_id: 3\n")), 5);
  EXPECT_EQ(error_line(source("root:\n  title: no type\n")), 4);
  EXPECT_EQ(error_line(source("root: just text\n")), 3);
  EXPECT_EQ(error_line(source("image: {width: 320}\n")), 3);                                     // no root
  EXPECT_EQ(error_line(source("")), 3);                                                          // empty format
  EXPECT_EQ(error_line(source("extra: 1\nroot:\n  type: Plot\n")), 3);
  EXPECT_EQ(error_line(source("root:\n  type: Plot\n  children: 7\n")), 5);
}

TEST_F(NymphBindTest, ImageOptionErrors) {
  EXPECT_EQ(error_line(source("image:\n  width: 5\nroot:\n  type: Plot\n")), 4);
  EXPECT_EQ(error_line(source("image:\n  height: 99999\nroot:\n  type: Plot\n")), 4);
  EXPECT_EQ(error_line(source("image:\n  scale: 0\nroot:\n  type: Plot\n")), 4);
  EXPECT_EQ(error_line(source("image:\n  width: wide\nroot:\n  type: Plot\n")), 4);
  EXPECT_EQ(error_line(source("image:\n  colour: red\nroot:\n  type: Plot\n")), 4);
  EXPECT_EQ(error_line(source("image:\n  theme: neon\nroot:\n  type: Plot\n")), 4);
  EXPECT_EQ(error_line(source("image:\n  width: 20\n  padding: 10\nroot:\n  type: Plot\n")), 4);
}

TEST_F(NymphBindTest, YamlSyntaxErrorIsPositioned) {
  EXPECT_GE(error_line(source("root:\n  type: Plot\n  title: [unclosed\n")), 5);
  EXPECT_GE(error_line(source("root:\n\ttype: Plot\n")), 3);
}

TEST_F(NymphBindTest, CsvErrorsAreReportedAtTheCsvRow) {
  // source(): data header on line 3 + (format lines) + 1
  const std::string format = "root:\n  type: Plot\n  children:\n    - type: PlotLine\n      ys: $b\n";
  EXPECT_EQ(error_line(source(format, "a,b\n1,2\n3,oops\n")), 11); // not a number
  EXPECT_EQ(error_line(source(format, "a,b\n1,2\n3\n")), 11);      // ragged row
  // A text column nobody plots as numbers is fine.
  EXPECT_EQ(error_line(source(format, "a,b\nx,2\ny,4\n")), 0);
}

TEST_F(NymphBindTest, SelfReferencingAnchorDoesNotHang) {
  EXPECT_NE(error_line(source("root: &loop\n  type: VerticalLayout\n  children:\n    - *loop\n")), 0);
}
