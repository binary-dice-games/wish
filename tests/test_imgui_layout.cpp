// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <imgui/imgui_layout.hpp>
#include <imgui/imgui_renderer.hpp>
#include <imgui/imgui_ui_renderer.hpp>
#include <server/registry.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"

#include <imgui.h>

using namespace bdg::bison;
using bdg::wish::context;
using bdg::wish::ensure_arranged;
using bdg::wish::imgui_renderer;
using bdg::wish::measure_node;
using bdg::wish::ui_element;

// ── Test fixture ──────────────────────────────────────────────────────────────
//
// Same headless ImGuiContext fixture pattern as test_imgui_renderer.cpp's
// ImguiRendererTest.

class ImguiLayoutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    io.Fonts->SetTexID(ImTextureID{1});
    io.MousePos = ImVec2(10.0f, 10.0f);
    sess_ = std::make_unique<context>("imgui_layout_test"_key);
    renderer_ = std::make_unique<imgui_renderer>();
  }

  void TearDown() override {
    renderer_.reset();
    sess_.reset();
    ImGui::DestroyContext(ctx_);
    ctx_ = nullptr;
  }

  // Render fn() inside a plain full-screen test window.
  void in_window(const std::function<void()>& fn) {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Always);
    ImGui::Begin(
        "TestWindow",
        nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar);
    fn();
    ImGui::End();
  }

  ImGuiContext* ctx_ = nullptr;
  std::unique_ptr<context> sess_;
  std::unique_ptr<imgui_renderer> renderer_;
};

// ── measure_node: unregistered leaves fall back to last_rendered_size() ─────
//
// Every leaf class (Label, Button, ProgressBar, Image, Plot, SliderFloat,
// and anything added in the future) is deliberately absent from
// measure_dispatch_fns() -- see that table's own doc comment in
// imgui_layout.cpp. Its own natural size instead comes from
// ui_element::last_rendered_size(), populated generically by
// imgui_renderer::render_node() every time a node actually renders. These
// tests cover that generic mechanism once, rather than one test per leaf
// class re-deriving the same formula measure_fns used to hardcode.

TEST_F(ImguiLayoutTest, MeasureNodeReturnsZeroForNeverRenderedLeaf) {
  // A node that has never actually been rendered has no real size to fall
  // back to yet -- {0,0} is the documented bootstrap default, self-correcting
  // the very next time this node renders for real (see the next test).
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"a fairly long label"})");

  renderer_->begin_frame();
  bdg::wish::natural_size sz{};
  in_window([&] { sz = measure_node(*renderer_, *map[""], *sess_); });
  renderer_->end_frame();

  EXPECT_FLOAT_EQ(sz.x, 0.0f);
  EXPECT_FLOAT_EQ(sz.y, 0.0f);
}

TEST_F(ImguiLayoutTest, MeasureNodeMatchesRealSizeAfterOneRealRender) {
  // Render the leaf for real once (capturing ImGui's own actual item rect
  // as ground truth via GetItemRectSize()), then confirm a later
  // measure_node() call picks up exactly that real size -- for a leaf class
  // with no custom formula at all. Button is a convenient example (it has a
  // real, non-trivial shape: label text + frame padding), but this is
  // testing the generic last_rendered_size() plumbing, not anything
  // Button-specific.
  auto map = bdg::wish::import_json(R"({"type":"Button","label":"a fairly long label"})");
  auto& node = *map[""];

  renderer_->begin_frame();
  ImVec2 rendered_sz{};
  in_window([&] {
    renderer_->render_node(node, *sess_);
    rendered_sz = ImGui::GetItemRectSize();
  });
  renderer_->end_frame();

  renderer_->begin_frame();
  bdg::wish::natural_size measured_sz{};
  in_window([&] { measured_sz = measure_node(*renderer_, node, *sess_); });
  renderer_->end_frame();

  EXPECT_FLOAT_EQ(measured_sz.x, rendered_sz.x);
  EXPECT_FLOAT_EQ(measured_sz.y, rendered_sz.y);
}

// ── measure_node: VerticalLayout sums fixed-height children + spacing ────────

TEST_F(ImguiLayoutTest, VerticalLayoutOfThreeFixedHeightButtonsSumsHeights) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","spacing":5,"children":{
      "a":{"type":"Button","label":"A","height":20},
      "b":{"type":"Button","label":"B","height":30},
      "c":{"type":"Button","label":"C","height":40}
  }})");

  renderer_->begin_frame();
  bdg::wish::natural_size sz{};
  in_window([&] { sz = measure_node(*renderer_, *map[""], *sess_); });
  renderer_->end_frame();

  EXPECT_FLOAT_EQ(sz.y, 20.0f + 30.0f + 40.0f + 5.0f * 2.0f);
}

// ── measure_node: Table respects an explicit outer_width/outer_height ───────

TEST_F(ImguiLayoutTest, MeasureTableRespectsExplicitOuterHeightOverRowCount) {
  // Regression test: a Table with an explicit "outer_height" (its own
  // render-time ImGui outer-size param -- distinct from the wish
  // Layout-hint "height" field that decides whether this Table is an
  // auto/fixed/stretch child of its parent) must contribute that fixed
  // size to its parent's own auto-sizing, not row-count*row-height
  // arithmetic. Getting this wrong made an unwrapped parent VerticalLayout
  // (e.g. zip_tool's "main") measure as tall as however many rows the
  // table currently has, pushing later siblings (a button row) far past
  // the window and into overlapping the table's own (correctly clipped)
  // content.
  std::string children;
  for (int i = 0; i < 20; ++i)
    children += "\"r" + std::to_string(i) + "\":{\"type\":\"TableRow\"},";
  auto desc = R"({"type":"Table","columns":1,"outer_height":50,"children":{)" + children +
      R"("c":{"type":"TableColumn","label":"Col"}}})";
  auto map = bdg::wish::import_json(desc);

  renderer_->begin_frame();
  bdg::wish::natural_size sz{};
  in_window([&] { sz = measure_node(*renderer_, *map[""], *sess_); });
  renderer_->end_frame();

  EXPECT_FLOAT_EQ(sz.y, 50.0f);
}

TEST_F(ImguiLayoutTest, MeasureTableRowHeightIncludesCellPadding) {
  // Regression test: measure_table()'s row-count fallback (no explicit
  // "outer_height") previously used GetTextLineHeightWithSpacing() alone,
  // undercounting each row's real footprint by 2*style.CellPadding.y --
  // ImGui's own TableNextRow()/TableEndRow() add that padding on top of the
  // row content height. For an auto-height Table that is a direct
  // (unwrapped) child of a Window, that shortfall directly inflated the
  // Window's own scrollable content region, producing a spurious few-pixel
  // vertical scrollbar.
  auto desc = R"({"type":"Table","columns":1,"children":{
      "r0":{"type":"TableRow"},"r1":{"type":"TableRow"},"r2":{"type":"TableRow"},
      "c":{"type":"TableColumn","label":"Col"}
  }})";
  auto map = bdg::wish::import_json(desc);

  renderer_->begin_frame();
  bdg::wish::natural_size sz{};
  float row_h{};
  in_window([&] {
    sz = measure_node(*renderer_, *map[""], *sess_);
    row_h = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().CellPadding.y * 2.0f;
  });
  renderer_->end_frame();

  EXPECT_FLOAT_EQ(sz.y, row_h * 3.0f);
}

// ── measure_node: every leaf class actually used as an auto Layout child ────

// ── IconThenLabelRow: file_explorer/file_dialog's actual row-cell shape ─────
//
// file_browser_utils.cpp's make_name_cell() builds a HorizontalLayout of an
// auto-size-to-font Image (icon) followed by a Label (filename). Both now
// have real formula-based measure_fns (measure_image()/measure_label() in
// imgui_layout.cpp) rather than the generic last_rendered_size() fallback --
// promoted specifically because make_name_cell() is called fresh on every
// navigate/sort/select (file_explorer.cpp rebuilds its row children from
// scratch), so a brand-new Image/Label never has a prior real render to fall
// back to, and last_rendered_size() would stay {0,0} forever in practice,
// not just for one bootstrap frame.

TEST_F(ImguiLayoutTest, IconThenLabelRowDoesNotOverlapOnFirstFrame) {
  // Regression test for the actual file_explorer bug: a brand-new icon+label
  // row, measured/arranged WITHOUT ever having rendered before (matching
  // file_explorer's real usage -- a fresh HorizontalLayout/Image/Label
  // instantiated by make_name_cell() on every row rebuild). Before Image/
  // Label got real measure_fns, a never-rendered node's last_rendered_size()
  // fallback was {0,0}, so the icon's arranged width was 0 and the label's
  // arranged x coincided with the icon's own position instead of starting
  // after it -- and since these are fresh objects every rebuild, this was
  // not a one-frame startup glitch, it reproduced on essentially every
  // interaction.
  auto map = bdg::wish::import_json(R"({"type":"HorizontalLayout","spacing":6,"children":{
      "icon":{"type":"Image","src":"res/icons/file.png"},
      "name":{"type":"Label","text":"clang-format"}
  }})");
  auto& root = *map[""];
  auto& icon = *map["icon"];
  auto& name = *map["name"];
  icon["__auto_size_to_font__"_key] = true;

  // Single frame, no prior render at all.
  renderer_->begin_frame();
  in_window([&] { ensure_arranged(*renderer_, root, *sess_); });
  renderer_->end_frame();

  float line = ImGui::GetTextLineHeight();
  EXPECT_FLOAT_EQ(icon.arranged_pos().x, name.arranged_pos().x - line - 6.0f);
  EXPECT_GE(name.arranged_pos().x, icon.arranged_pos().x + icon.arranged_size().x);
}

TEST_F(ImguiLayoutTest, IconThenLabelRowDoesNotOverlap) {
  // Same shape, but exercised across two frames (render once for real, then
  // re-arrange) to confirm the formula-based measure and the real rendered
  // size agree -- i.e. measure_image()/measure_label() aren't just correct
  // in isolation, they match what render_image()/render_label() actually
  // draw.
  auto map = bdg::wish::import_json(R"({"type":"HorizontalLayout","spacing":6,"children":{
      "icon":{"type":"Image","src":"res/icons/file.png"},
      "name":{"type":"Label","text":"clang-format"}
  }})");
  auto& root = *map[""];
  auto& icon = *map["icon"];
  auto& name = *map["name"];
  icon["__auto_size_to_font__"_key] = true;

  renderer_->begin_frame();
  in_window([&] { renderer_->render_node(root, *sess_); });
  renderer_->end_frame();

  renderer_->begin_frame();
  in_window([&] { ensure_arranged(*renderer_, root, *sess_); });
  renderer_->end_frame();

  float line = ImGui::GetTextLineHeight();
  EXPECT_FLOAT_EQ(icon.arranged_pos().x, name.arranged_pos().x - line - 6.0f);
  EXPECT_GE(name.arranged_pos().x, icon.arranged_pos().x + icon.arranged_size().x);
}

TEST_F(ImguiLayoutTest, ContentExtentIsSmallEvenWhenSelfHealedWithLargeAmbientAvail) {
  // Regression test for the bug introduced by pin_cursor_to_arranged_bottom()
  // itself: a HorizontalLayout with no stretch/fill child (e.g. this same
  // per-row icon-then-label cell, self-healing inside a Table cell where the
  // ambient GetContentRegionAvail() is "whatever's left in the whole
  // scrollable table region", not this one row) must NOT report its
  // arranged_size() as its true content bottom -- that's the *given* space
  // (here, in_window()'s ~580px-tall test window, standing in for a table's
  // large remaining scroll region), not what its children actually used.
  // content_extent() is the fix: it must stay close to the label's own
  // single-line height regardless of how large the ambient avail was.
  auto map = bdg::wish::import_json(R"({"type":"HorizontalLayout","spacing":6,"children":{
      "icon":{"type":"Image","src":"res/icons/file.png"},
      "name":{"type":"Label","text":"clang-format"}
  }})");
  auto& root = *map[""];
  (*map["icon"])["__auto_size_to_font__"_key] = true;

  // Formula-based measure_image()/measure_label() make a prior real render
  // unnecessary for this row's own sizing, but ensure_arranged() is still
  // exercised the same way a self-healed Table cell would call it.
  renderer_->begin_frame();
  in_window([&] { ensure_arranged(*renderer_, root, *sess_); });
  renderer_->end_frame();

  // in_window() is 800x600 -- arranged_size().y would be on that order if
  // this test were (incorrectly) reading it instead of content_extent().
  EXPECT_LT(root.content_extent().y, 100.0f);
  EXPECT_GT(root.arranged_size().y, 100.0f);
}

TEST_F(ImguiLayoutTest, StretchChildContributesZeroToParentNaturalHeight) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "fixed":{"type":"Button","label":"A","height":20},
      "stretch":{"type":"Button","label":"B","height":-1}
  }})");

  // This test is about stretch-vs-fixed sizing math, not spacing -- zero out
  // ItemSpacing so effective_spacing()'s active-style fallback (imgui_layout.cpp)
  // doesn't add an extra gap into the expected round numbers below.
  ImGui::GetStyle().ItemSpacing = ImVec2(0.0f, 0.0f);

  renderer_->begin_frame();
  bdg::wish::natural_size sz{};
  in_window([&] { sz = measure_node(*renderer_, *map[""], *sess_); });
  renderer_->end_frame();

  // Only the fixed child's height counts -- the stretch child wants to fill
  // whatever's left over, not define its parent's natural size.
  EXPECT_FLOAT_EQ(sz.y, 20.0f);
}

// ── arrange_node: fixed + stretch children placed correctly ─────────────────

TEST_F(ImguiLayoutTest, ArrangeNodePlacesFixedAndStretchChildren) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "fixed":{"type":"Button","label":"A","height":100},
      "stretch":{"type":"Button","label":"B","height":-1}
  }})");
  auto& root = *map[""];
  auto& fixed = *map["fixed"];
  auto& stretch = *map["stretch"];

  // This test is about stretch-vs-fixed placement math, not spacing -- see
  // the identical zeroing in StretchChildContributesZeroToParentNaturalHeight
  // above.
  ImGui::GetStyle().ItemSpacing = ImVec2(0.0f, 0.0f);

  renderer_->begin_frame();
  in_window([&] {
    measure_node(*renderer_, root, *sess_);
    bdg::wish::arrange_node(*renderer_, root, ImVec2(0.0f, 0.0f), ImVec2(0.0f, 300.0f), *sess_);
  });
  renderer_->end_frame();

  EXPECT_FLOAT_EQ(fixed.arranged_pos().y, 0.0f);
  EXPECT_FLOAT_EQ(fixed.arranged_size().y, 100.0f);
  EXPECT_FLOAT_EQ(stretch.arranged_pos().y, 100.0f);
  EXPECT_FLOAT_EQ(stretch.arranged_size().y, 200.0f);
}

// ── ensure_arranged: self-heal without going through render_window ──────────

TEST_F(ImguiLayoutTest, EnsureArrangedOnBareNodePopulatesStashFromCursor) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "a":{"type":"Label","text":"x"}
  }})");
  auto& root = *map[""];

  renderer_->begin_frame();
  in_window([&] {
    ImVec2 pos = ImGui::GetCursorPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    bool was_fresh = ensure_arranged(*renderer_, root, *sess_);
    EXPECT_FALSE(was_fresh);
    EXPECT_FLOAT_EQ(root.arranged_pos().x, pos.x);
    EXPECT_FLOAT_EQ(root.arranged_pos().y, pos.y);
    EXPECT_FLOAT_EQ(root.arranged_size().x, avail.x);
    EXPECT_FLOAT_EQ(root.arranged_size().y, avail.y);
  });
  renderer_->end_frame();
}

TEST_F(ImguiLayoutTest, EnsureArrangedSelfHealsStaleFrame) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "a":{"type":"Label","text":"x"}
  }})");
  auto& root = *map[""];

  renderer_->begin_frame();
  in_window([&] {
    // Stamp a stash as if written by some earlier frame -- never the real
    // current ImGui::GetFrameCount() -- so ensure_arranged() must treat it
    // as stale rather than trusting it.
    root.set_arranged_rect({0.0f, 0.0f}, {123.0f, 456.0f}, ImGui::GetFrameCount() - 1);
    bool was_fresh = ensure_arranged(*renderer_, root, *sess_);
    EXPECT_FALSE(was_fresh);
    EXPECT_NE(root.arranged_size().y, 456.0f);
  });
  renderer_->end_frame();
}

TEST_F(ImguiLayoutTest, EnsureArrangedNoOpWhenAlreadyFreshThisFrame) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "a":{"type":"Label","text":"x"}
  }})");
  auto& root = *map[""];

  renderer_->begin_frame();
  in_window([&] {
    EXPECT_FALSE(ensure_arranged(*renderer_, root, *sess_));
    // Second call this same frame: the stash is already fresh, so this is a
    // pure no-op -- exactly the case that lets a container's top-down pass
    // and a self-healing descendant agree without redoing work.
    EXPECT_TRUE(ensure_arranged(*renderer_, root, *sess_));
  });
  renderer_->end_frame();
}

// ── Font-metric parity: measure must match what render actually draws at ────

TEST_F(ImguiLayoutTest, LabelWithExplicitFontSizeMeasuresConsistentlyWithRender) {
  // No real TTF backend is attached in this headless fixture (get_or_load_font
  // always returns nullptr -- see imgui_renderer::get_or_load_font()), so this
  // can't assert against a genuinely different font's line height. What it
  // does assert: measure_node()'s font push/pop (via resolve_element_font())
  // must exactly mirror render_node()'s, so whichever font either resolves
  // to, the two stay consistent -- the actual regression this test guards
  // against is measure and render silently drifting apart.
  auto map = bdg::wish::import_json(R"({"type":"Label","text":"hi","font_size":32.0})");
  auto& node = *map[""];

  renderer_->begin_frame();
  ImVec2 rendered_sz{};
  in_window([&] {
    renderer_->render_node(node, *sess_);
    rendered_sz = ImGui::GetItemRectSize();
  });
  renderer_->end_frame();

  renderer_->begin_frame();
  bdg::wish::natural_size measured_sz{};
  in_window([&] { measured_sz = measure_node(*renderer_, node, *sess_); });
  renderer_->end_frame();

  EXPECT_FLOAT_EQ(measured_sz.x, rendered_sz.x);
  EXPECT_FLOAT_EQ(measured_sz.y, rendered_sz.y);
}
