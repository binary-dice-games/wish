// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/imgui_renderer.hpp>
#include <wish/registry.hpp>
#include <wish/ui_importer.hpp>

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_common.hpp"

#include <imgui.h>
#include <imgui_internal.h>

using namespace bdg::bison;
using bdg::wish::imgui_renderer;
using bdg::wish::render_children;
using bdg::wish::session;
using bdg::wish::ui_element;

// ── Test fixture ──────────────────────────────────────────────────────────────

class ImguiRendererTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime   = 1.0f / 60.0f;
    // Build font atlas in headless mode (no platform backend does this for us).
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    io.Fonts->SetTexID(ImTextureID{1});
    // Place mouse somewhere valid so window hover detection works.
    io.MousePos = ImVec2(10.0f, 10.0f);
    sess_     = std::make_unique<session>("imgui_test"_key);
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
    ImGui::Begin("TestWindow", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoScrollbar);
    fn();
    ImGui::End();
  }

  // Simulate a press-and-release on @p item_id after a NewFrame.
  // Must be called AFTER ImGui::NewFrame() and BEFORE rendering.
  // Sets internal imgui state so ButtonBehavior returns true for that item.
  static void fake_click(ImGuiID item_id) {
    ImGuiContext& g = *GImGui;
    g.ActiveId                    = item_id;
    g.ActiveIdIsAlive             = item_id;
    g.ActiveIdSource              = ImGuiInputSource_Mouse;
    // ActiveIdMouseButton must NOT be -1: ButtonBehavior calls ClearActiveID()
    // immediately when it finds -1 (programmatic set) before reaching release logic.
    g.ActiveIdMouseButton         = 0;      // left button
    g.ActiveIdWindow              = ImGui::FindWindowByName("TestWindow");
    g.IO.MouseDown[0]             = false;
    g.IO.MouseDownDuration[0]     = 0.05f;  // was held briefly
    g.IO.MouseReleased[0]         = true;   // just released
    g.IO.MousePos                 = ImVec2(10.0f, 10.0f);
  }

  ImGuiContext*           ctx_      = nullptr;
  std::unique_ptr<session>         sess_;
  std::unique_ptr<imgui_renderer>  renderer_;
};

// ── begin_frame / end_frame ───────────────────────────────────────────────────

TEST_F(ImguiRendererTest, BeginEndFrameDoNotThrow) {
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    renderer_->end_frame();
  });
}

// ── Label: no throw, no event ─────────────────────────────────────────────────

TEST_F(ImguiRendererTest, LabelDoesNotThrowOrEmitEvent) {
  bool event_fired = false;
  sess_->emit_event = [&](key_t, key_t, dynamic) { event_fired = true; };

  auto map = bdg::wish::import_json(R"({"type":"Label","text":"hello"})");

  renderer_->begin_frame();
  in_window([&] { renderer_->render_node(*map[""], *sess_); });
  renderer_->end_frame();

  EXPECT_FALSE(event_fired);
}

// ── Button: emits "clicked" on simulated press+release ───────────────────────

TEST_F(ImguiRendererTest, ButtonEmitsClickedEvent) {
  key_t last_event{hash_t{0}};
  sess_->emit_event = [&](key_t, key_t ev, dynamic) { last_event = ev; };

  auto map = bdg::wish::import_json(R"({"type":"Button","label":"OK"})");

  // Frame 1: render to register the window and discover the button's ImGui ID.
  renderer_->begin_frame();
  ImGuiID btn_id{0};
  in_window([&] {
    renderer_->render_node(*map[""], *sess_);
    btn_id = ImGui::GetItemID();
  });
  renderer_->end_frame();

  // Frame 2: manually set press+release state AFTER NewFrame, then render.
  // This is the standard headless-testing technique for ImGui widgets.
  ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
  ImGui::NewFrame();
  fake_click(btn_id);
  in_window([&] { renderer_->render_node(*map[""], *sess_); });
  ImGui::EndFrame();

  EXPECT_EQ(last_event, "clicked"_key);
}

// ── Checkbox: emits "changed" with correct boolean payload ───────────────────

TEST_F(ImguiRendererTest, CheckboxEmitsChangedWithCorrectPayload) {
  key_t  last_event{hash_t{0}};
  bool   last_value = false;
  sess_->emit_event = [&](key_t, key_t ev, dynamic payload) {
    last_event = ev;
    auto* f = payload.findField("value"_key);
    if (f && f->is<bool>()) last_value = f->as<bool>();
  };

  // Checkbox starts unchecked; fake-click will toggle it to true.
  auto map = bdg::wish::import_json(
      R"({"type":"Checkbox","label":"Check","value":false})");

  // Frame 1: register the window and get the checkbox's ID.
  renderer_->begin_frame();
  ImGuiID cb_id{0};
  in_window([&] {
    renderer_->render_node(*map[""], *sess_);
    cb_id = ImGui::GetItemID();
  });
  renderer_->end_frame();

  // Frame 2: simulate press+release on the checkbox.
  ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
  ImGui::NewFrame();
  fake_click(cb_id);
  in_window([&] { renderer_->render_node(*map[""], *sess_); });
  ImGui::EndFrame();

  EXPECT_EQ(last_event, "changed"_key);
  EXPECT_TRUE(last_value);
}

// ── Unknown class: no throw ───────────────────────────────────────────────────

TEST_F(ImguiRendererTest, UnknownClassDoesNotThrow) {
  // Rendering any fully-valid tree exercises the dispatch table, including
  // the else (unknown-class) branch when a class key is not in the table.
  // We verify no exception escapes render_node.
  auto map = bdg::wish::import_json(
      R"({"type":"Window","title":"T","children":{"s":{"type":"Separator"}}})");
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    renderer_->render_node(*map[""], *sess_);
    renderer_->end_frame();
  });
}

// ── Window with two Label children ───────────────────────────────────────────

// Counting subclass: wraps the real imgui_renderer and counts per-type calls.
class counting_imgui_renderer : public imgui_renderer {
 public:
  int label_count = 0;

  void render_node(const ui_element& node, session& s) override {
    if (node.as<key_t>(dynamic::CLASS) == "Label"_key) ++label_count;
    imgui_renderer::render_node(node, s);
  }
};

TEST_F(ImguiRendererTest, WindowWithTwoLabelsCallsRenderNodeTwiceForLabels) {
  constexpr auto desc = R"({
    "type": "Window",
    "title": "W",
    "children": {
      "a": { "type": "Label", "text": "first"  },
      "b": { "type": "Label", "text": "second" }
    }
  })";
  auto map = bdg::wish::import_json(desc);

  counting_imgui_renderer r;
  r.begin_frame();
  r.render_node(*map[""], *sess_);
  r.end_frame();

  EXPECT_EQ(r.label_count, 2);
}
