// MIT License © 2025 Binary Dice Games
//
// Tests for sdl3_renderer's native (ABI-driven) automation support -- the
// SDL3 counterpart to test_automation_query.cpp's web-renderer coverage (see
// src/automation/DESIGN.md's "Native (ABI-based) automation" section).
// Headless: SDL's "dummy" video driver + "software" render backend, same
// setup as test_sdl3_renderer.cpp, so these run in CI with no real display.
#include <gtest/gtest.h>

#include <automation/automation_backend.hpp>
#include <automation/automation_query.hpp>
#include <context/context.hpp>
#include <sdl/sdl3_renderer.hpp>
#include <server/registry.hpp>
#include <ui/ui_importer.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <thread>

using bdg::wish::context;
using bdg::wish::sdl3_renderer;

using namespace bdg::bison;

namespace {

// import_json() alone never assigns __wish_id -- mirrors
// test_automation_query.cpp's identical helper.
void assign_wish_ids(bdg::wish::ui_tree& tree) {
  uint32_t next_id = 1;
  for (auto& [path, elem] : tree)
    (*elem)["__wish_id"_key] = bdg::bison::key_t{next_id++};
}

// Polls up to ~250ms for poll_events() to report activity (SDL's dummy
// driver still delivers pushed events through the normal event queue, but
// not necessarily on the very first poll) -- mirrors
// test_automation_query.cpp's WebRendererAutomationTest poll loops.
bool wait_for_activity(sdl3_renderer& renderer) {
  for (int i = 0; i < 50; ++i) {
    if (renderer.poll_events())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

class Sdl3AutomationTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    bdg::wish::register_all(); // DisplayName attributes for the "class" field
  }

  void SetUp() override {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    renderer_ = std::make_unique<sdl3_renderer>();
    renderer_->setup();
  }

  void TearDown() override {
    renderer_->teardown();
    renderer_.reset();
  }

  std::unique_ptr<sdl3_renderer> renderer_;
};

} // namespace

// ── as_automation_backend() ─────────────────────────────────────────────────

TEST_F(Sdl3AutomationTest, AsAutomationBackendReturnsSelf) {
  EXPECT_EQ(
      renderer_->as_automation_backend(),
      static_cast<bdg::wish::automation::automation_backend*>(renderer_.get()));
}

// ── hit-test capture / query_tree() ─────────────────────────────────────────

TEST_F(Sdl3AutomationTest, QueryTreeIncludesRenderedButtonWithNonNullRect) {
  context ctx{"automation_test"_key};
  auto tree = bdg::wish::import_json(R"({"type":"Window","title":"T",
      "children":{"ok":{"type":"Button","label":"OK"}}})");
  assign_wish_ids(tree);
  ctx.ui_objects = std::move(tree);

  renderer_->begin_frame();
  renderer_->render_node(*ctx.ui_objects[""], ctx);
  renderer_->end_frame();

  auto fut = renderer_->query_tree(1, "");
  renderer_->service_automation_queries(ctx);
  auto json_text = fut.get();

  auto j = nlohmann::json::parse(json_text);
  EXPECT_EQ(j["request_id"], 1);

  bool found = false;
  for (auto& w : j["widgets"]) {
    if (w["path"] == "ok") {
      found = true;
      EXPECT_EQ(w["class"], "Button");
      EXPECT_EQ(w["label"], "OK");
      EXPECT_FALSE(w["rect"].is_null());
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(Sdl3AutomationTest, QueryTreeRootFilterRestrictsToNodeAndDescendants) {
  context ctx{"automation_test2"_key};
  auto tree = bdg::wish::import_json(R"({"type":"Window","title":"T",
      "children":{"ok":{"type":"Button","label":"OK"},"cancel":{"type":"Button","label":"Cancel"}}})");
  assign_wish_ids(tree);
  ctx.ui_objects = std::move(tree);

  renderer_->begin_frame();
  renderer_->render_node(*ctx.ui_objects[""], ctx);
  renderer_->end_frame();

  auto fut = renderer_->query_tree(2, "ok");
  renderer_->service_automation_queries(ctx);
  auto j = nlohmann::json::parse(fut.get());

  ASSERT_EQ(j["widgets"].size(), 1u);
  EXPECT_EQ(j["widgets"][0]["path"], "ok");
}

// ── screenshot capture ───────────────────────────────────────────────────────

TEST_F(Sdl3AutomationTest, CaptureScreenshotReturnsNonEmptyPngBytes) {
  renderer_->begin_frame();
  auto fut = renderer_->capture_screenshot();
  renderer_->end_frame(); // drains the pending screenshot before present

  auto png = fut.get();
  ASSERT_GE(png.size(), 8u);
  // PNG magic bytes: 0x89 'P' 'N' 'G' \r \n 0x1A \n
  EXPECT_EQ(png[0], 0x89);
  EXPECT_EQ(png[1], static_cast<uint8_t>('P'));
  EXPECT_EQ(png[2], static_cast<uint8_t>('N'));
  EXPECT_EQ(png[3], static_cast<uint8_t>('G'));
}

// ── input injection ──────────────────────────────────────────────────────────
//
// poll_events() reports "activity" for every mouse-button/key event
// unconditionally, and for mouse motion once it clears the debounce
// threshold (see sdl3_renderer::mouse_motion_significant()) -- the first
// motion always counts, since there is no baseline yet. This is the same
// signal wish::server::render_loop() uses to decide whether to draw a frame,
// so it's a meaningful (if indirect) check that SDL_PushEvent() actually
// delivered the synthetic event through the real event queue.

TEST_F(Sdl3AutomationTest, InjectMouseMoveRegistersAsPollEventsActivity) {
  renderer_->inject_mouse_move(10.0f, 20.0f);
  EXPECT_TRUE(wait_for_activity(*renderer_));
}

TEST_F(Sdl3AutomationTest, InjectMouseButtonRegistersAsPollEventsActivity) {
  renderer_->inject_mouse_button(0, true);
  EXPECT_TRUE(wait_for_activity(*renderer_));
}

TEST_F(Sdl3AutomationTest, InjectKeyRegistersAsPollEventsActivity) {
  renderer_->inject_key(SDLK_A, true);
  EXPECT_TRUE(wait_for_activity(*renderer_));
}

TEST_F(Sdl3AutomationTest, InjectTextIsDrainedByBeginFrameWithoutThrowing) {
  renderer_->inject_text("hi");
  EXPECT_NO_THROW(renderer_->begin_frame());
  renderer_->end_frame();
}
