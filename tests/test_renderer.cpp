// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/renderer.hpp>
#include <wish/registry.hpp>
#include <wish/ui_importer.hpp>

#include <string>
#include <vector>

using namespace bdg::bison;
using bdg::wish::null_renderer;
using bdg::wish::render_children;
using bdg::wish::renderer;
using bdg::wish::session;
using bdg::wish::ui_element;

// ── Test infrastructure ───────────────────────────────────────────────────────

// Renderer that counts calls and records visited class keys; recurses into
// children so that tree-wide counts can be verified.
class counting_renderer : public renderer {
 public:
  int count = 0;
  std::vector<key_t> visited;

  void begin_frame() override {}

  void render_node(const ui_element& node, session& s) override {
    ++count;
    visited.push_back(node.as<key_t>(dynamic::CLASS));
    render_children(*this, node, s);
  }

  void end_frame() override {}
};

class RendererTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
    sess_ = std::make_unique<session>("renderer_test"_key);
  }

  session& sess() { return *sess_; }

 private:
  std::unique_ptr<session> sess_;
};

// ── null_renderer ─────────────────────────────────────────────────────────────

TEST_F(RendererTest, NullRendererDoesNotThrow) {
  auto map = bdg::wish::import_json(R"({"type":"Window"})");
  null_renderer r;
  EXPECT_NO_THROW(r.render_node(*map[""], sess()));
}

// ── render_children call counts ───────────────────────────────────────────────

TEST_F(RendererTest, NoChildrenCallsRenderNodeZeroTimes) {
  auto map = bdg::wish::import_json(R"({"type":"Window"})");
  counting_renderer r;
  render_children(r, *map[""], sess());
  EXPECT_EQ(r.count, 0);
}

TEST_F(RendererTest, TwoIndexedChildrenCallsRenderNodeTwice) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": [
      { "type": "Label", "text": "first" },
      { "type": "Label", "text": "second" }
    ]
  })";
  auto map = bdg::wish::import_json(desc);
  counting_renderer r;
  render_children(r, *map[""], sess());
  EXPECT_EQ(r.count, 2);
}

TEST_F(RendererTest, TwoNamedChildrenCallsRenderNodeTwice) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": {
      "alpha": { "type": "Label", "text": "A" },
      "beta":  { "type": "Label", "text": "B" }
    }
  })";
  auto map = bdg::wish::import_json(desc);
  counting_renderer r;
  render_children(r, *map[""], sess());
  EXPECT_EQ(r.count, 2);
}

// ── Declaration order ─────────────────────────────────────────────────────────

// Keys "zzz" and "aaa" hash to different values; the importer stamps order
// 0 / 1 in declaration sequence, so "zzz" (order 0) must be visited first.
TEST_F(RendererTest, NamedChildrenVisitedInDeclarationOrder) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": {
      "zzz": { "type": "Label", "text": "first" },
      "aaa": { "type": "Label", "text": "second" }
    }
  })";
  auto map = bdg::wish::import_json(desc);

  std::vector<key_t> order;
  map[""]->for_each_child_ordered([&](key_t k, ui_element&) {
    order.push_back(k);
  });

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "zzz"_key);
  EXPECT_EQ(order[1], "aaa"_key);
}

TEST_F(RendererTest, IndexedChildrenVisitedInIndexOrder) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": [
      { "type": "Label", "text": "first" },
      { "type": "Label", "text": "second" }
    ]
  })";
  auto map = bdg::wish::import_json(desc);

  std::vector<std::string> texts;
  map[""]->for_each_child_ordered([&](key_t, ui_element& child) {
    auto* f = child.findField("text"_key);
    if (f && f->is<std::string>()) texts.push_back(f->as<std::string>());
  });

  ASSERT_EQ(texts.size(), 2u);
  EXPECT_EQ(texts[0], "first");
  EXPECT_EQ(texts[1], "second");
}

// ── Lifecycle no-ops ─────────────────────────────────────────────────────────

TEST_F(RendererTest, SetupTeardownAndShouldQuitHaveNoOpDefaults) {
  null_renderer r;
  EXPECT_NO_THROW(r.setup());
  EXPECT_NO_THROW(r.teardown());
  EXPECT_FALSE(r.should_quit());
}

// ── Three-level tree ──────────────────────────────────────────────────────────

// Window → VerticalLayout → [Label, Button]
// Counting renderer recurses, so total render_node calls = 4.
TEST_F(RendererTest, ThreeLevelTreeAccumulatesCorrectCount) {
  constexpr auto desc = R"({
    "type": "Window",
    "children": {
      "body": {
        "type": "VerticalLayout",
        "children": {
          "lbl": { "type": "Label",  "text": "hello" },
          "btn": { "type": "Button", "label": "ok"   }
        }
      }
    }
  })";
  auto map = bdg::wish::import_json(desc);
  counting_renderer r;
  r.render_node(*map[""], sess());

  // Window + VerticalLayout + Label + Button = 4
  EXPECT_EQ(r.count, 4);
}
