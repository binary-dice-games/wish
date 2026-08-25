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

// ── measure_node: Table falls back to last_rendered_size(), like every ─────
// ── other composite/leaf with no registered measure_fn ──────────────────────
//
// Table has no bespoke measure_fn (see imgui_layout.cpp's comment on why it
// was removed -- it duplicated ImGui's own TableNextRow()/TableEndRow() row-
// height arithmetic in a second, physically separate place, which drifted
// out of sync at least once already). Its own natural size instead comes
// from the same generic ui_element::last_rendered_size() fallback every
// other composite (TextEditor, Plot) and leaf (Button, ProgressBar) already
// uses: one frame of lag on a genuinely brand-new Table, self-correcting
// immediately after a real render -- exercised generically by
// MeasureNodeMatchesRealSizeAfterOneRealRender above (Table needs no
// dedicated copy of that test; the mechanism is class-agnostic).

TEST_F(ImguiLayoutTest, MeasureTableBeforeAnyRealRenderReturnsZero) {
  // Mirrors MeasureNodeReturnsZeroForNeverRenderedLeaf's reasoning for
  // Table specifically: no formula, no real render yet, so no size to fall
  // back to -- the honest {0,0} bootstrap default, not a guess.
  auto desc = R"({"type":"Table","columns":1,"children":{
      "r0":{"type":"TableRow"},"c":{"type":"TableColumn","label":"Col"}
  }})";
  auto map = bdg::wish::import_json(desc);

  renderer_->begin_frame();
  bdg::wish::natural_size sz{};
  in_window([&] { sz = measure_node(*renderer_, *map[""], *sess_); });
  renderer_->end_frame();

  EXPECT_FLOAT_EQ(sz.x, 0.0f);
  EXPECT_FLOAT_EQ(sz.y, 0.0f);
}

TEST_F(ImguiLayoutTest, MeasureTableAfterRealRenderMatchesActualRenderedRowHeight) {
  // The row-height*CellPadding arithmetic the deleted measure_table() used
  // to hand-compute is now just whatever ImGui's own BeginTable()/
  // TableNextRow()/EndTable() actually drew -- captured for real via
  // GetItemRectSize() and confirmed to match a later measure_node() call,
  // the same pattern as MeasureNodeMatchesRealSizeAfterOneRealRender.
  //
  // Deliberately does NOT re-derive ImGui's own row-height formula
  // (GetTextLineHeightWithSpacing() + CellPadding.y*2, or whatever else
  // TableNextRow()/TableEndRow() actually do) to compute an expected value
  // to compare against -- hand-deriving that formula a second time in this
  // test is exactly the duplication bug measure_table() itself used to
  // have (it was off by one CellPadding term for a long time). The
  // multi-row-taller-than-one-row check below is the only invariant this
  // test needs: real content genuinely has more rows.
  auto one_row_map = bdg::wish::import_json(
      R"({"type":"Table","columns":1,"children":{"r0":{"type":"TableRow"},"c":{"type":"TableColumn","label":"Col"}}})");
  auto three_row_map = bdg::wish::import_json(R"({"type":"Table","columns":1,"children":{
      "r0":{"type":"TableRow"},"r1":{"type":"TableRow"},"r2":{"type":"TableRow"},
      "c":{"type":"TableColumn","label":"Col"}
  }})");

  renderer_->begin_frame();
  ImVec2 one_row_sz{};
  in_window([&] {
    renderer_->render_node(*one_row_map[""], *sess_);
    one_row_sz = ImGui::GetItemRectSize();
  });
  renderer_->end_frame();

  renderer_->begin_frame();
  ImVec2 three_row_sz{};
  in_window([&] {
    renderer_->render_node(*three_row_map[""], *sess_);
    three_row_sz = ImGui::GetItemRectSize();
  });
  renderer_->end_frame();

  EXPECT_GT(three_row_sz.y, one_row_sz.y);

  renderer_->begin_frame();
  bdg::wish::natural_size measured_sz{};
  in_window([&] { measured_sz = measure_node(*renderer_, *three_row_map[""], *sess_); });
  renderer_->end_frame();

  EXPECT_FLOAT_EQ(measured_sz.x, three_row_sz.x);
  EXPECT_FLOAT_EQ(measured_sz.y, three_row_sz.y);
}

// ── measure_node: every leaf class actually used as an auto Layout child ────

// ── IconThenLabelRow: mc/file_dialog's actual row-cell shape ─────
//
// file_browser_utils.cpp's make_name_cell() builds a HorizontalLayout of an
// auto-size-to-font Image (icon) followed by a Label (filename), rebuilt
// from scratch on every navigate/sort/select. Neither Image nor Label has
// its own measure_fn (see imgui_layout.cpp's measure_dispatch_fns()
// comment) -- a fresh instance's natural size comes from the generic
// last_rendered_size() fallback, {0,0} until it has rendered for real once.
// That's a real, expected discrepancy at the pure arrange level (see
// IconThenLabelRowArrangePosCanUnderMeasureBeforeAnyRealRender below), but
// not a visible one: render_horizontal_layout() places an unhinted sibling
// via ImGui's own real SameLine() cursor advance, not this pre-computed
// position, so the label lands after the icon's actual drawn width on
// every frame including the very first -- covered by
// FreshIconThenLabelRowDoesNotOverlapOnFirstRealRender in
// test_imgui_renderer.cpp, which exercises a real render (this file's
// fixture only arranges, never renders for real).

TEST_F(ImguiLayoutTest, IconThenLabelRowArrangePosCanUnderMeasureBeforeAnyRealRender) {
  // Documents the one caveat the fallback (vs. a bespoke formula) accepts:
  // arranged_pos()/arranged_size() for a never-rendered auto icon+label
  // pair reflect {0,0} natural sizes, so the label's arranged x sits right
  // on top of the icon's arranged x -- this is fine precisely because
  // nothing downstream trusts arranged_pos() for *positioning* an unhinted
  // sibling anymore (see the section comment above). This test exists so a
  // future reader doesn't mistake the gap between "arranged_pos looks
  // wrong" and "nothing actually overlaps on screen" for a regression.
  auto map = bdg::wish::import_json(R"({"type":"HorizontalLayout","spacing":6,"children":{
      "icon":{"type":"Image","src":"res/icons/file.png"},
      "name":{"type":"Label","text":"clang-format"}
  }})");
  auto& root = *map[""];
  auto& icon = *map["icon"];
  auto& name = *map["name"];
  icon["__auto_size_to_font__"_key] = true;

  renderer_->begin_frame();
  in_window([&] { ensure_arranged(*renderer_, root, *sess_); });
  renderer_->end_frame();

  EXPECT_FLOAT_EQ(icon.arranged_size().x, 0.0f);
  EXPECT_FLOAT_EQ(icon.arranged_pos().x, name.arranged_pos().x - 6.0f);
}

TEST_F(ImguiLayoutTest, IconThenLabelRowArrangeMatchesRealSizeAfterOneRealRender) {
  // Exercised across two frames (render once for real, then re-arrange) to
  // confirm the generic last_rendered_size() fallback and the real rendered
  // size agree once a real render has happened -- self-corrects within one
  // frame, exactly the documented trade-off for any fallback-eligible
  // class.
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
  // Regression test for the bug pin_cursor_to_arranged_bottom() (now
  // superseded by content_extent()-based BeginChild sizing, see
  // src/imgui/DESIGN.md's "Why content_extent, not arranged_size") existed
  // to fix: a HorizontalLayout with no stretch/fill child (e.g. this same
  // per-row icon-then-label cell, self-healing inside a Table cell where
  // the ambient GetContentRegionAvail() is "whatever's left in the whole
  // scrollable table region", not this one row) must NOT report its
  // arranged_size() as its true content bottom -- that's the *given* space
  // (here, in_window()'s ~580px-tall test window, standing in for a
  // table's large remaining scroll region), not what its children actually
  // used. Rendered once for real first so content_extent() reflects real
  // (small) content rather than the vacuously-small {0,0} a never-rendered
  // fallback would also satisfy.
  auto map = bdg::wish::import_json(R"({"type":"HorizontalLayout","spacing":6,"children":{
      "icon":{"type":"Image","src":"res/icons/file.png"},
      "name":{"type":"Label","text":"clang-format"}
  }})");
  auto& root = *map[""];
  (*map["icon"])["__auto_size_to_font__"_key] = true;

  renderer_->begin_frame();
  in_window([&] { renderer_->render_node(root, *sess_); });
  renderer_->end_frame();

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

TEST_F(ImguiLayoutTest, EnsureArrangedSkipsRedundantMeasureWhenAlreadyMeasuredThisFrame) {
  // Reproduces render_window()'s real sequence: an enclosing pass measures
  // (but, unlike arrange, never arranges -- Window has no arrange_dispatch_fns
  // entry) the subtree first, then a descendant layout's own ensure_arranged()
  // self-heal runs. Before this fix, ensure_arranged() re-measured
  // unconditionally, doubling every measure_node() walk each frame -- see
  // src/imgui/DESIGN.md's "Hook points" section.
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "a":{"type":"Label","text":"x"}
  }})");
  auto& root = *map[""];

  renderer_->begin_frame();
  in_window([&] {
    measure_node(*renderer_, root, *sess_);
    EXPECT_TRUE(root.is_measure_fresh(ImGui::GetFrameCount()));
    EXPECT_FALSE(root.is_arrange_fresh(ImGui::GetFrameCount()));

    // Stamp a sentinel measured size so a redundant re-measure inside
    // ensure_arranged() would be observable -- measure_node() on this
    // content always recomputes a real (non-sentinel) size.
    root.set_measured_size({777.0f, 888.0f}, ImGui::GetFrameCount());

    bool was_fresh = ensure_arranged(*renderer_, root, *sess_);
    EXPECT_FALSE(was_fresh); // still needed to arrange, just not re-measure
    EXPECT_TRUE(root.is_arrange_fresh(ImGui::GetFrameCount()));
    EXPECT_FLOAT_EQ(root.measured_size().x, 777.0f);
    EXPECT_FLOAT_EQ(root.measured_size().y, 888.0f);
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
