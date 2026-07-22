// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include <imgui/imgui_renderer.hpp>
#include <imgui/imgui_ui_renderer.hpp>
#include <server/registry.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"

#include <imgui.h>
#include <imgui_internal.h>

using namespace bdg::bison;
using bdg::wish::imgui_renderer;
using bdg::wish::context;
using bdg::wish::ui_element;

// ── Test fixture ──────────────────────────────────────────────────────────────
//
// Mirrors ImguiRendererTest (test_imgui_renderer.cpp) exactly for setup/
// teardown/in_window(). Drag-and-drop needs a real, natural press-move-
// release mouse sequence (not the single-frame fake_click() forced-ActiveId
// trick used for plain button clicks elsewhere) because ImGui's own
// UpdateMouseInputs() -- called every NewFrame() -- is what tracks
// MouseClickedPos/MouseDragMaxDistanceSqr, which BeginDragDropSource()
// consults to decide whether the drag threshold was crossed. Forcing
// g.ActiveId directly (as fake_click does) would skip that bookkeeping.
class DragDropTest : public ::testing::Test {
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
    io.MouseDown[0] = false;
    sess_ = std::make_unique<context>("drag_drop_test"_key);
    renderer_ = std::make_unique<imgui_renderer>();
  }

  void TearDown() override {
    renderer_.reset();
    sess_.reset();
    ImGui::DestroyContext(ctx_);
    ctx_ = nullptr;
  }

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

  // One NewFrame()/EndFrame() cycle with the given mouse state, rendering
  // both @p src and @p tgt (in that order) inside the test window.
  void render_frame(ui_element& src, ui_element& tgt, ImVec2 mouse_pos, bool mouse_down) {
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = mouse_pos;
    bool was_down = io.MouseDown[0];
    io.MouseDown[0] = mouse_down;
    io.MouseReleased[0] = was_down && !mouse_down;
    ImGui::NewFrame();
    in_window([&] {
      renderer_->render_node(src, *sess_);
      renderer_->render_node(tgt, *sess_);
    });
    ImGui::EndFrame();
  }

  // Drains sess_->pending_events the same way the real server render loop
  // does after each frame (see enqueue_event()'s doc comment).
  void drain_events() {
    for (auto& ev : sess_->pending_events)
      if (sess_->emit_event)
        sess_->emit_event(ev.id, ev.event_name, ev.payload);
    sess_->pending_events.clear();
  }

  ImGuiContext* ctx_ = nullptr;
  std::unique_ptr<context> sess_;
  std::unique_ptr<imgui_renderer> renderer_;
};

namespace {

// Builds a Button acting as a drag source at a given screen position, and a
// second Button (further down the window) acting as a drop target. Returns
// {source, target}; both already carry their "__path__"/render-ready state
// via import_json.
std::pair<std::shared_ptr<ui_element>, std::shared_ptr<ui_element>> make_source_and_target(
    const std::string& drag_type, const std::string& drag_payload, const std::string& drop_type) {
  auto src_map = bdg::wish::import_json(R"({"type":"Button","label":"Src"})");
  auto tgt_map = bdg::wish::import_json(R"({"type":"Button","label":"Tgt"})");
  auto& src = *src_map[""];
  auto& tgt = *tgt_map[""];
  src["drag_type"_key] = drag_type;
  src["drag_payload"_key] = drag_payload;
  tgt["drop_type"_key] = drop_type;
  return {src_map[""], tgt_map[""]};
}

} // namespace

// ── Defaults: no fields set -> no behavior change ────────────────────────────

TEST_F(DragDropTest, ElementsWithNoDragOrDropFieldsSetNeverEmitDropped) {
  bool event_fired = false;
  sess_->emit_event = [&](bdg::bison::key_t, bdg::bison::key_t, dynamic) { event_fired = true; };

  auto map = bdg::wish::import_json(R"({"type":"Window","title":"W",
      "children":{"a":{"type":"Button","label":"A"},"b":{"type":"Button","label":"B"}}})");

  renderer_->begin_frame();
  in_window([&] { renderer_->render_node(*map[""], *sess_); });
  renderer_->end_frame();
  drain_events();

  EXPECT_FALSE(event_fired);
}

// ── Full drag gesture: press on source, drag to target, release ─────────────

TEST_F(DragDropTest, DraggingSourceOntoMatchingTargetEmitsDroppedWithPayload) {
  auto [src, tgt] = make_source_and_target("GenieAsset", "TextureAsset|textures/x.png", "GenieAsset");

  bdg::bison::key_t last_widget{hash_t{0}};
  bdg::bison::key_t last_event{hash_t{0}};
  std::string last_type;
  std::string last_payload;
  sess_->emit_event = [&](bdg::bison::key_t widget, bdg::bison::key_t ev, dynamic payload) {
    last_widget = widget;
    last_event = ev;
    if (auto* f = payload.findField("type"_key); f && f->is<std::string>())
      last_type = f->as<std::string>();
    if (auto* f = payload.findField("payload"_key); f && f->is<std::string>())
      last_payload = f->as<std::string>();
  };

  // Frame 1: establish the layout (source above target) and locate each
  // widget's rect -- neither button pressed yet.
  renderer_->begin_frame();
  ImVec2 src_center{0, 0}, tgt_center{0, 0};
  in_window([&] {
    renderer_->render_node(*src, *sess_);
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    src_center = ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    renderer_->render_node(*tgt, *sess_);
    mn = ImGui::GetItemRectMin();
    mx = ImGui::GetItemRectMax();
    tgt_center = ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
  });
  renderer_->end_frame();
  ASSERT_NE(src_center.y, tgt_center.y); // sanity: two distinct, non-overlapping rows

  // Frame 2: press on the source -- ImGui's own UpdateMouseInputs() (run
  // inside NewFrame()) detects the up->down transition, sets ActiveId to the
  // hovered source button, and records MouseClickedPos for the drag-distance
  // check the next frame's BeginDragDropSource() call needs.
  render_frame(*src, *tgt, src_center, /*mouse_down=*/true);

  // Frame 3: move (still held) onto the target, well past ImGui's drag
  // threshold. ActiveId is still the source (ImGui keeps it while the mouse
  // stays down, regardless of current hover), so BeginDragDropSource()
  // succeeds and SetDragDropPayload() runs; the target is now hovered, so
  // BeginDragDropTarget()/AcceptDragDropPayload() see the same payload
  // within this same frame -- but delivery itself only happens on release.
  render_frame(*src, *tgt, tgt_center, /*mouse_down=*/true);
  drain_events();
  EXPECT_FALSE(last_event.id) << "drop must not fire before the mouse is released";

  // Frame 4: release over the target -- this is the frame AcceptDragDropPayload()
  // reports the payload as delivered.
  render_frame(*src, *tgt, tgt_center, /*mouse_down=*/false);
  drain_events();

  EXPECT_EQ(last_widget, tgt->as<bdg::bison::key_t>("__wish_id"_key));
  EXPECT_EQ(last_event, "dropped"_key);
  EXPECT_EQ(last_type, "GenieAsset");
  EXPECT_EQ(last_payload, "TextureAsset|textures/x.png");
}

// ── Type mismatch: source and target disagree -> no event ───────────────────

TEST_F(DragDropTest, MismatchedDragAndDropTypesNeverEmitDropped) {
  auto [src, tgt] = make_source_and_target("GenieAsset", "payload", "SomethingElse");

  bool event_fired = false;
  sess_->emit_event = [&](bdg::bison::key_t, bdg::bison::key_t, dynamic) { event_fired = true; };

  ImVec2 src_center{0, 0}, tgt_center{0, 0};
  renderer_->begin_frame();
  in_window([&] {
    renderer_->render_node(*src, *sess_);
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    src_center = ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    renderer_->render_node(*tgt, *sess_);
    mn = ImGui::GetItemRectMin();
    mx = ImGui::GetItemRectMax();
    tgt_center = ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
  });
  renderer_->end_frame();

  render_frame(*src, *tgt, src_center, true);
  render_frame(*src, *tgt, tgt_center, true);
  render_frame(*src, *tgt, tgt_center, false);
  drain_events();

  EXPECT_FALSE(event_fired);
}

// ── Release without ever dragging onto a target -> no event ─────────────────

TEST_F(DragDropTest, PressAndReleaseOnSourceWithoutDraggingDoesNotEmitDropped) {
  auto [src, tgt] = make_source_and_target("GenieAsset", "payload", "GenieAsset");

  bool event_fired = false;
  sess_->emit_event = [&](bdg::bison::key_t, bdg::bison::key_t, dynamic) { event_fired = true; };

  ImVec2 src_center{0, 0};
  renderer_->begin_frame();
  in_window([&] {
    renderer_->render_node(*src, *sess_);
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    src_center = ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    renderer_->render_node(*tgt, *sess_);
  });
  renderer_->end_frame();

  // Press and release on the source itself -- never crosses the drag
  // threshold, so BeginDragDropSource() never succeeds this whole gesture.
  render_frame(*src, *tgt, src_center, true);
  render_frame(*src, *tgt, src_center, false);
  drain_events();

  EXPECT_FALSE(event_fired);
}
