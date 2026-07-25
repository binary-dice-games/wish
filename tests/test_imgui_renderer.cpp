// MIT License © 2025 Binary Dice Games
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
using bdg::wish::render_children;
using bdg::wish::context;
using bdg::wish::ui_element;

// ── Test fixture ──────────────────────────────────────────────────────────────

class ImguiRendererTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Hermetic: never load/save a real imgui.ini. Without this, a stale
    // entry left on disk by an earlier run (window titles here are static
    // test-fixture strings, several sharing stable_id() == "0" since these
    // ad-hoc trees never go through ui_template's __wish_id assignment)
    // could silently override a test's expected fresh-window pos/size,
    // now that render_window uses ImGuiCond_FirstUseEver instead of Once.
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    // Build font atlas in headless mode (no platform backend does this for us).
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    io.Fonts->SetTexID(ImTextureID{1});
    // Place mouse somewhere valid so window hover detection works.
    io.MousePos = ImVec2(10.0f, 10.0f);
    sess_ = std::make_unique<context>("imgui_test"_key);
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

  // Simulate a press-and-release on @p item_id after a NewFrame.
  // Must be called AFTER ImGui::NewFrame() and BEFORE rendering.
  // Sets internal imgui state so ButtonBehavior returns true for that item.
  static void fake_click(ImGuiID item_id) {
    ImGuiContext& g = *GImGui;
    g.ActiveId = item_id;
    g.ActiveIdIsAlive = item_id;
    g.ActiveIdSource = ImGuiInputSource_Mouse;
    // ActiveIdMouseButton must NOT be -1: ButtonBehavior calls ClearActiveID()
    // immediately when it finds -1 (programmatic set) before reaching release logic.
    g.ActiveIdMouseButton = 0; // left button
    g.ActiveIdWindow = ImGui::FindWindowByName("TestWindow");
    g.IO.MouseDown[0] = false;
    g.IO.MouseDownDuration[0] = 0.05f; // was held briefly
    g.IO.MouseReleased[0] = true; // just released
    g.IO.MousePos = ImVec2(10.0f, 10.0f);
  }

  ImGuiContext* ctx_ = nullptr;
  std::unique_ptr<context> sess_;
  std::unique_ptr<imgui_renderer> renderer_;
};

// ── stable_id: deterministic ImGui ID across runs ────────────────────────────

TEST_F(ImguiRendererTest, StableIdSameForSamePathAcrossRebuilds) {
  // Two independently-built trees (as if from two separate process runs)
  // with a child at the same dot-path must agree on stable_id, even though
  // the child's other fields differ.
  auto map1 = bdg::wish::import_json(R"({"type":"Window","title":"W",
      "children":{"body":{"type":"Label","text":"x"}}})");
  auto map2 = bdg::wish::import_json(R"({"type":"Window","title":"W",
      "children":{"body":{"type":"Label","text":"y"}}})");

  EXPECT_EQ(bdg::wish::stable_id(*map1["body"]), bdg::wish::stable_id(*map2["body"]));
}

TEST_F(ImguiRendererTest, StableIdDiffersForDifferentPaths) {
  auto map = bdg::wish::import_json(R"({"type":"Window","title":"W",
      "children":{"a":{"type":"Label","text":"x"},"b":{"type":"Label","text":"y"}}})");

  EXPECT_NE(bdg::wish::stable_id(*map["a"]), bdg::wish::stable_id(*map["b"]));
}

TEST_F(ImguiRendererTest, StableIdFallsBackToWishIdWhenPathEmpty) {
  // The root node's "__path__" is "" (see import_json), which is not a
  // usable stable identity -- stable_id() must fall back to __wish_id,
  // matching pre-existing behavior for nodes without a template-assigned
  // dot-path (e.g. form-internal elements).
  auto map = bdg::wish::import_json(R"({"type":"Window","title":"W"})");
  auto& root = *map[""];
  root["__wish_id"_key] = bdg::bison::key_t{hash_t{42}};

  EXPECT_EQ(bdg::wish::stable_id(root), "42");
}

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
  sess_->emit_event = [&](bdg::bison::key_t, bdg::bison::key_t, dynamic) { event_fired = true; };

  auto map = bdg::wish::import_json(R"({"type":"Label","text":"hello"})");

  renderer_->begin_frame();
  in_window([&] { renderer_->render_node(*map[""], *sess_); });
  renderer_->end_frame();

  EXPECT_FALSE(event_fired);
}

// ── Button: emits "clicked" on simulated press+release ───────────────────────

TEST_F(ImguiRendererTest, ButtonEmitsClickedEvent) {
  bdg::bison::key_t last_event{hash_t{0}};
  sess_->emit_event = [&](bdg::bison::key_t, bdg::bison::key_t ev, dynamic) { last_event = ev; };

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

  // Drain pending_events: simulates the server render loop's post-frame dispatch.
  for (auto& ev : sess_->pending_events)
    if (sess_->emit_event)
      sess_->emit_event(ev.id, ev.event_name, ev.payload);
  sess_->pending_events.clear();

  EXPECT_EQ(last_event, "clicked"_key);
}

// ── Checkbox: emits "changed" with correct boolean payload ───────────────────

TEST_F(ImguiRendererTest, CheckboxEmitsChangedWithCorrectPayload) {
  bdg::bison::key_t last_event{hash_t{0}};
  bool last_value = false;
  sess_->emit_event = [&](bdg::bison::key_t, bdg::bison::key_t ev, dynamic payload) {
    last_event = ev;
    auto* f = payload.findField("value"_key);
    if (f && f->is<bool>())
      last_value = f->as<bool>();
  };

  // Checkbox starts unchecked; fake-click will toggle it to true.
  auto map = bdg::wish::import_json(R"({"type":"Checkbox","label":"Check","value":false})");

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

  // Drain pending_events: simulates the server render loop's post-frame dispatch.
  for (auto& ev : sess_->pending_events)
    if (sess_->emit_event)
      sess_->emit_event(ev.id, ev.event_name, ev.payload);
  sess_->pending_events.clear();

  EXPECT_EQ(last_event, "changed"_key);
  EXPECT_TRUE(last_value);
}

// ── MenuButton: opens a popup on click, exposing its children ───────────────

// Captures the MenuItem's ImGui item id from inside render_menu_button()'s
// still-open popup scope. EndPopup() (an End() call) restores
// g.LastItemData to whatever it was before BeginPopup() was entered, so
// ImGui::GetItemID() read *after* render_node() returns reports the
// trigger Button's id again, not the MenuItem's -- the same End()
// last-item-restore behavior documented on
// WindowRestoresFloatingSizeAfterUndock above, just hitting GetItemID()
// instead of GetID().
class menu_item_capturing_renderer : public imgui_renderer {
 public:
  ImGuiID menu_item_id{0};

  void render_node(const ui_element& node, const context& s) override {
    imgui_renderer::render_node(node, s);
    if (node.as<bdg::bison::key_t>(dynamic::CLASS) == "MenuItem"_key)
      menu_item_id = ImGui::GetItemID();
  }
};

TEST_F(ImguiRendererTest, MenuButtonOpensPopupAndRendersChildOnClick) {
  auto map = bdg::wish::import_json(
      R"({"type":"MenuButton","label":"Create","children":{"item":{"type":"MenuItem","label":"Object"}}})");
  // Root has no "__path__"/"__wish_id" in this ad-hoc tree (see
  // StableIdFallsBackToWishIdWhenPathEmpty above), so stable_id() falls
  // back to the default key_t{} -- computed via the real helper rather than
  // hardcoded so this doesn't silently drift from stable_id()'s own logic.
  std::string root_id = bdg::wish::stable_id(*map[""]);
  std::string popup_id = "Create###" + root_id;

  menu_item_capturing_renderer r;

  // Frame 1: popup starts closed -- only the trigger Button renders.
  //
  // Checking IsPopupOpen() here is fiddlier than a plain widget check for
  // two reasons:
  //  - render_node() (imgui_renderer.cpp) wraps every node's dispatch in
  //    ImGui::PushID(stable_id(node)), so render_menu_button() computed
  //    the popup's id one ID-stack level deeper than "TestWindow" alone --
  //    replicate that same PushID here so IsPopupOpen() hashes the exact
  //    id OpenPopup() used.
  //  - ImGui::IsPopupOpen(name) also requires an active window scope on
  //    top of that (it calls ImGuiWindow::GetID() on the current window,
  //    same caveat documented on
  //    ModalWindowStaysOpenAcrossFramesWithoutReopening above), so it's
  //    checked from inside in_window() rather than after end_frame().
  r.begin_frame();
  ImGuiID btn_id{0};
  bool popup_open_before_click = true;
  in_window([&] {
    r.render_node(*map[""], *sess_);
    btn_id = ImGui::GetItemID();
    ImGui::PushID(root_id.c_str());
    popup_open_before_click = ImGui::IsPopupOpen(popup_id.c_str());
    ImGui::PopID();
  });
  r.end_frame();
  EXPECT_FALSE(popup_open_before_click);

  // Frame 2: simulate press+release on the trigger button -- opens the
  // popup and renders its MenuItem child in this same frame (standard
  // ImGui OpenPopup()-then-BeginPopup() idiom needs no extra frame delay).
  ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
  ImGui::NewFrame();
  fake_click(btn_id);
  bool popup_open_after_click = false;
  in_window([&] {
    r.render_node(*map[""], *sess_);
    ImGui::PushID(root_id.c_str());
    popup_open_after_click = ImGui::IsPopupOpen(popup_id.c_str());
    ImGui::PopID();
  });
  ImGui::EndFrame();

  EXPECT_TRUE(popup_open_after_click);
  // menu_item_capturing_renderer captured the MenuItem's id from inside
  // the still-open popup scope, before EndPopup() could reset it.
  EXPECT_NE(r.menu_item_id, btn_id);
  EXPECT_NE(r.menu_item_id, ImGuiID{0});
}

// ── Unknown class: no throw ───────────────────────────────────────────────────

TEST_F(ImguiRendererTest, UnknownClassDoesNotThrow) {
  // Rendering any fully-valid tree exercises the dispatch table, including
  // the else (unknown-class) branch when a class key is not in the table.
  // We verify no exception escapes render_node.
  auto map = bdg::wish::import_json(R"({"type":"Window","title":"T","children":{"s":{"type":"Separator"}}})");
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

  void render_node(const ui_element& node, const context& s) override {
    if (node.as<bdg::bison::key_t>(dynamic::CLASS) == "Label"_key)
      ++label_count;
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

// ── Step 10: layout support ───────────────────────────────────────────────────

TEST_F(ImguiRendererTest, VerticalLayoutWithThreeLabelsDoesNotThrow) {
  constexpr auto desc = R"({
    "type": "Window",
    "title": "VL",
    "children": {
      "vl": {
        "type": "VerticalLayout",
        "spacing": 4.0,
        "children": {
          "a": { "type": "Label", "text": "one"   },
          "b": { "type": "Label", "text": "two"   },
          "c": { "type": "Label", "text": "three" }
        }
      }
    }
  })";
  auto map = bdg::wish::import_json(desc);
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    renderer_->render_node(*map[""], *sess_);
    renderer_->end_frame();
  });
}

TEST_F(ImguiRendererTest, HorizontalLayoutWithThreeButtonsDoesNotThrow) {
  constexpr auto desc = R"({
    "type": "Window",
    "title": "HL",
    "children": {
      "hl": {
        "type": "HorizontalLayout",
        "spacing": 4.0,
        "children": {
          "a": { "type": "Button", "label": "A" },
          "b": { "type": "Button", "label": "B" },
          "c": { "type": "Button", "label": "C" }
        }
      }
    }
  })";
  auto map = bdg::wish::import_json(desc);
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    renderer_->render_node(*map[""], *sess_);
    renderer_->end_frame();
  });
}

TEST_F(ImguiRendererTest, NestedLayoutTreeRendersWithoutError) {
  constexpr auto desc = R"({
    "type": "Window",
    "title": "Nested",
    "children": {
      "vl": {
        "type": "VerticalLayout",
        "children": {
          "row0": {
            "type": "HorizontalLayout",
            "children": {
              "lbl": { "type": "Label",  "text": "Name" },
              "btn": { "type": "Button", "label": "OK"  }
            }
          },
          "row1": {
            "type": "HorizontalLayout",
            "children": {
              "lbl": { "type": "Label",  "text": "Value" },
              "btn": { "type": "Button", "label": "Cancel" }
            }
          }
        }
      }
    }
  })";
  auto map = bdg::wish::import_json(desc);
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    renderer_->render_node(*map[""], *sess_);
    renderer_->end_frame();
  });
}

TEST_F(ImguiRendererTest, VerticalLayoutSpacingZeroAndNonzeroDoNotThrow) {
  constexpr auto desc_zero = R"({
    "type": "Window", "title": "V0",
    "children": {
      "vl": {
        "type": "VerticalLayout", "spacing": 0.0,
        "children": {
          "a": { "type": "Label", "text": "A" },
          "b": { "type": "Label", "text": "B" }
        }
      }
    }
  })";
  constexpr auto desc_eight = R"({
    "type": "Window", "title": "V8",
    "children": {
      "vl": {
        "type": "VerticalLayout", "spacing": 8.0,
        "children": {
          "a": { "type": "Label", "text": "A" },
          "b": { "type": "Label", "text": "B" }
        }
      }
    }
  })";
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    auto m0 = bdg::wish::import_json(desc_zero);
    renderer_->render_node(*m0[""], *sess_);
    renderer_->end_frame();
  });
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    auto m8 = bdg::wish::import_json(desc_eight);
    renderer_->render_node(*m8[""], *sess_);
    renderer_->end_frame();
  });
}

// ── Table, TableColumn, TableRow ──────────────────────────────────────────────

TEST_F(ImguiRendererTest, TableWithHeadersAndRowsDoesNotThrow) {
  constexpr auto desc = R"({
    "type": "Window", "title": "tbl",
    "children": {
      "t": { "type": "Table", "id": "t_basic", "columns": 3,
             "flags": 1985, "headers": true,
        "children": {
          "ca": { "type": "TableColumn", "label": "Name"     },
          "cb": { "type": "TableColumn", "label": "Price"    },
          "cc": { "type": "TableColumn", "label": "Category" },
          "r0": { "type": "TableRow", "children": {
            "c0": { "type": "Label", "text": "Widget" },
            "c1": { "type": "Label", "text": "$9.99"  },
            "c2": { "type": "Label", "text": "HW"     }
          }},
          "r1": { "type": "TableRow", "children": {
            "c0": { "type": "Label", "text": "Gizmo" },
            "c1": { "type": "Label", "text": "$4.50" },
            "c2": { "type": "Label", "text": "SW"    }
          }}
        }
      }
    }
  })";
  auto map = bdg::wish::import_json(desc);
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    in_window([&] { renderer_->render_node(*map[""], *sess_); });
    renderer_->end_frame();
  });
}

TEST_F(ImguiRendererTest, TableCellsRenderWithoutThrowIncludingButton) {
  // Verify that a Table containing TableColumn, TableRow, and a Button in a
  // cell renders across multiple frames without crashing.  Button click events
  // are covered by ButtonEmitsClickedEvent; here we focus on table plumbing.
  constexpr auto desc = R"({
    "type": "Table", "id": "t_btn", "columns": 2,
    "flags": 1920, "headers": true,
    "children": {
      "ca": { "type": "TableColumn", "label": "Name"   },
      "cb": { "type": "TableColumn", "label": "Action" },
      "r0": { "type": "TableRow", "children": {
        "c0": { "type": "Label",  "text": "Item A"  },
        "c1": { "type": "Button", "label": "Select" }
      }}
    }
  })";
  auto map = bdg::wish::import_json(desc);

  EXPECT_NO_THROW({
    renderer_->begin_frame();
    in_window([&] { renderer_->render_node(*map[""], *sess_); });
    renderer_->end_frame();
  });
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    in_window([&] { renderer_->render_node(*map[""], *sess_); });
    renderer_->end_frame();
  });
}

// ── Docking: undock restores pre-dock floating size ──────────────────────────

TEST_F(ImguiRendererTest, WindowRestoresFloatingSizeAfterUndock) {
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  auto map = bdg::wish::import_json(R"({"type":"Window","title":"Dockable","width":400,"height":300})");
  auto& win = *map[""];
  auto wish_id =
      win.get_as<bdg::bison::key_t>("__wish_id"_key, bdg::bison::key_t{});
  // "###" (not "##") matches with_id()'s convention in imgui_ui_renderer.cpp:
  // the ID must depend only on wish_id, not the visible "Dockable" prefix,
  // so editing a window's title doesn't reset ImGui's per-window state
  // (position/size/dock/focus) -- see with_id()'s doc comment.
  std::string label = "Dockable###" + std::to_string(wish_id.id);

  // Render one floating frame: establishes the ImGuiCond_Once 400x300 size
  // and populates the hidden __float_width__/__float_height__ fields.
  renderer_->begin_frame();
  renderer_->render_node(win, *sess_);
  renderer_->end_frame();
  EXPECT_EQ(win.get_as<int32_t>("__float_width__"_key, 0), 400);
  EXPECT_EQ(win.get_as<int32_t>("__float_height__"_key, 0), 300);

  // Programmatically dock the window via DockBuilder (no simulated drag
  // needed -- imgui_internal.h is already included above).
  //
  // ImGui::GetID() hashes against GImGui->CurrentWindow, so it can only be
  // called inside a Begin()/End() scope; here we're between frames (right
  // after end_frame()), so CurrentWindow is null and GetID() would
  // dereference it unconditionally (no assert in release builds -- this
  // segfaulted before this fix, see git history for the investigation).
  // ImHashStr() computes the same kind of stable id without touching
  // CurrentWindow, which is all a docking id needs to be.
  ImGuiID dock_id = ImHashStr("TestDockSpace");
  ImGui::DockBuilderRemoveNode(dock_id);
  ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_None);
  ImGui::DockBuilderSetNodeSize(dock_id, ImVec2(800, 600));
  ImGui::DockBuilderDockWindow(label.c_str(), dock_id);
  ImGui::DockBuilderFinish(dock_id);

  // A live DockSpace() host is required for ImGui to merge the pending dock
  // request into a real dock node. Drive a bounded number of frames until
  // the window reports as docked.
  bool docked = false;
  for (int i = 0; i < 5 && !docked; ++i) {
    renderer_->begin_frame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(800, 600));
    ImGui::Begin(
        "Host",
        nullptr,
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove);
    ImGui::DockSpace(dock_id, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
    renderer_->render_node(win, *sess_);
    renderer_->end_frame();
    auto* w = ImGui::FindWindowByName(label.c_str());
    docked = w && w->DockIsActive;
  }
  ASSERT_TRUE(docked);
  ImVec2 docked_size = ImGui::FindWindowByName(label.c_str())->Size;
  EXPECT_NE(docked_size.x, 400.0f); // sanity: docking actually changed the size

  // Force undock via DockBuilder's documented idiom (dock to node 0).
  ImGui::DockBuilderDockWindow(label.c_str(), 0);

  bool restored = false;
  for (int i = 0; i < 3 && !restored; ++i) {
    renderer_->begin_frame();
    renderer_->render_node(win, *sess_);
    renderer_->end_frame();
    auto* w = ImGui::FindWindowByName(label.c_str());
    restored = w && !w->DockIsActive && w->Size.x == 400.0f && w->Size.y == 300.0f;
  }
  EXPECT_TRUE(restored);
}

// ── Modal: true input-blocking popup ─────────────────────────────────────────

TEST_F(ImguiRendererTest, ModalWindowSetsModalAndNoDockingFlags) {
  auto map = bdg::wish::import_json(R"({"type":"Window","title":"MB","modal":true})");
  auto& win = *map[""];
  std::string label = "MB###" + bdg::wish::stable_id(win);

  renderer_->begin_frame();
  bdg::wish::render_window(*renderer_, win, *sess_);
  renderer_->end_frame();

  auto* w = ImGui::FindWindowByName(label.c_str());
  ASSERT_NE(w, nullptr);
  EXPECT_TRUE(w->Flags & ImGuiWindowFlags_Modal);
  EXPECT_TRUE(w->Flags & ImGuiWindowFlags_NoDocking);
}

TEST_F(ImguiRendererTest, ModalWindowStaysOpenAcrossFramesWithoutReopening) {
  // OpenPopup() must be called exactly once (on activation), not every
  // frame -- render several frames and confirm the popup stays open and the
  // one-shot latch (__modal_opened__) stays true, with no crash from a
  // mismatched Begin/End or popup-stack assertion.
  auto map = bdg::wish::import_json(R"({"type":"Window","title":"MB","modal":true})");
  auto& win = *map[""];
  std::string label = "MB###" + bdg::wish::stable_id(win);

  // ImGui::IsPopupOpen(name) requires an active window scope (it calls
  // ImGuiWindow::GetID() on the current window), so it can only be checked
  // from inside a Begin/End pair -- same caveat documented above for
  // ImGui::GetID() between frames. Check it via render_window's own return
  // path instead: FindWindowByName() (used elsewhere in this file between
  // frames) plus the __modal_opened__ latch are sufficient to confirm the
  // popup opened once and stayed open, without crashing.
  (void)label;
  for (int i = 0; i < 3; ++i) {
    EXPECT_NO_THROW({
      renderer_->begin_frame();
      bdg::wish::render_window(*renderer_, win, *sess_);
      renderer_->end_frame();
    });
    EXPECT_TRUE(win.get_as<bool>("__modal_opened__"_key, false));
  }
  auto* w = ImGui::FindWindowByName(label.c_str());
  ASSERT_NE(w, nullptr);
  EXPECT_TRUE(w->WasActive);
}

TEST_F(ImguiRendererTest, NonClosableModalEmitsClosedWhenPopupClosedProgrammatically) {
  // No closable field, so the only way this modal closes is via
  // ImGui::CloseCurrentPopup() (e.g. an in-content OK/Cancel handler).
  // ClosePopupToLevel(0, true) is the same internal call CloseCurrentPopup()
  // makes; it's already used in this file (via imgui_internal.h) for
  // DockBuilder above.
  bdg::bison::key_t last_event{hash_t{0}};
  sess_->emit_event = [&](bdg::bison::key_t, bdg::bison::key_t ev, dynamic) { last_event = ev; };

  auto map = bdg::wish::import_json(R"({"type":"Window","title":"MB","modal":true})");
  auto& win = *map[""];
  win["__wish_id"_key] = bdg::bison::key_t{hash_t{99}};

  renderer_->begin_frame();
  bdg::wish::render_window(*renderer_, win, *sess_);
  renderer_->end_frame();
  EXPECT_TRUE(win.get_as<bool>("__modal_opened__"_key, false));

  ImGui::ClosePopupToLevel(0, true);

  renderer_->begin_frame();
  bdg::wish::render_window(*renderer_, win, *sess_);
  renderer_->end_frame();

  EXPECT_FALSE(win.get_as<bool>("__modal_opened__"_key, false));
  for (auto& ev : sess_->pending_events)
    if (sess_->emit_event)
      sess_->emit_event(ev.id, ev.event_name, ev.payload);
  sess_->pending_events.clear();
  EXPECT_EQ(last_event, "closed"_key);
}

TEST_F(ImguiRendererTest, ClosableModalDoesNotDoubleFireOnNormalFrame) {
  // Sanity check: a closable modal that nobody has closed must not emit
  // "closed" just from being rendered normally across frames.
  bool event_fired = false;
  sess_->emit_event = [&](bdg::bison::key_t, bdg::bison::key_t, dynamic) { event_fired = true; };

  auto map = bdg::wish::import_json(R"({"type":"Window","title":"MB","modal":true,"closable":true})");
  auto& win = *map[""];

  for (int i = 0; i < 2; ++i) {
    renderer_->begin_frame();
    bdg::wish::render_window(*renderer_, win, *sess_);
    renderer_->end_frame();
  }
  for (auto& ev : sess_->pending_events)
    if (sess_->emit_event)
      sess_->emit_event(ev.id, ev.event_name, ev.payload);
  sess_->pending_events.clear();

  EXPECT_FALSE(event_fired);
}

// ── Modal: app-requested close (message_box::request_close()'s mechanism) ────

TEST_F(ImguiRendererTest, RequestCloseFieldClosesPopupAndFiresClosedEvent) {
  // Mirrors what message_box::request_close() does: app code (running
  // outside any ImGui frame) can't call ImGui::CloseCurrentPopup() itself,
  // so it sets this hidden field instead and waits for render_window() to
  // act on it and confirm the close via a "closed" event.
  bdg::bison::key_t last_event{hash_t{0}};
  sess_->emit_event = [&](bdg::bison::key_t, bdg::bison::key_t ev, dynamic) { last_event = ev; };

  auto map = bdg::wish::import_json(R"({"type":"Window","title":"MB","modal":true})");
  auto& win = *map[""];

  renderer_->begin_frame();
  bdg::wish::render_window(*renderer_, win, *sess_);
  renderer_->end_frame();
  ASSERT_TRUE(win.get_as<bool>("__modal_opened__"_key, false));

  // Simulate an app handler's request_close(): set the field directly,
  // exactly as message_box::request_close() does via a session lock, with
  // no accompanying real ImGui call (that's render_window()'s job).
  win["__request_close__"_key] = true;

  // Frame where render_window() notices the request and calls
  // CloseCurrentPopup() -- must also mark the session dirty on its own
  // (nothing else does), or a caller relying on the dirty flag to decide
  // whether to render again would stall here forever.
  sess_->dirty.store(false, std::memory_order_release);
  renderer_->begin_frame();
  bdg::wish::render_window(*renderer_, win, *sess_);
  renderer_->end_frame();
  EXPECT_TRUE(sess_->dirty.load(std::memory_order_acquire));
  EXPECT_FALSE(win.get_as<bool>("__request_close__"_key, true));

  // Next frame: BeginPopupModal now reports closed; "closed" fires and the
  // one-shot latch resets, ready for this exact ID to be reopened cleanly.
  renderer_->begin_frame();
  bdg::wish::render_window(*renderer_, win, *sess_);
  renderer_->end_frame();
  for (auto& ev : sess_->pending_events)
    if (sess_->emit_event)
      sess_->emit_event(ev.id, ev.event_name, ev.payload);
  sess_->pending_events.clear();

  EXPECT_EQ(last_event, "closed"_key);
  EXPECT_FALSE(win.get_as<bool>("__modal_opened__"_key, true));
}

TEST_F(ImguiRendererTest, SecondModalReusingSameStableIdOpensCleanlyAfterProperClose) {
  // Regression test for the actual reported bug: a *second* modal instance
  // that reuses the first one's stable id (e.g. because
  // form::next_available_key() recycled the freed key -- see
  // message_box::rebuild()) must open normally on its very first render,
  // not require an unrelated input event before ImGui's popup stack
  // reconciles. This only holds if the first instance was closed the
  // proper way (ImGui::CloseCurrentPopup(), not just abandoned).
  auto map1 = bdg::wish::import_json(R"({"type":"Window","title":"First","modal":true})");
  auto& win1 = *map1[""];
  win1["__path__"_key] = std::string{"__test_modal_reuse__"};

  renderer_->begin_frame();
  bdg::wish::render_window(*renderer_, win1, *sess_);
  renderer_->end_frame();
  ASSERT_TRUE(win1.get_as<bool>("__modal_opened__"_key, false));

  // Properly close it -- the fixed request_close()/"__request_close__" path.
  win1["__request_close__"_key] = true;
  renderer_->begin_frame();
  bdg::wish::render_window(*renderer_, win1, *sess_);
  renderer_->end_frame();
  renderer_->begin_frame();
  bdg::wish::render_window(*renderer_, win1, *sess_);
  renderer_->end_frame();
  ASSERT_FALSE(win1.get_as<bool>("__modal_opened__"_key, true));
  // win1 is now abandoned (as remove_internal_objects() would do) -- never
  // rendered again, same as a real form after its "closed" event fires.

  auto map2 = bdg::wish::import_json(R"({"type":"Window","title":"Second","modal":true})");
  auto& win2 = *map2[""];
  win2["__path__"_key] = std::string{"__test_modal_reuse__"}; // same stable id as win1

  renderer_->begin_frame();
  bdg::wish::render_window(*renderer_, win2, *sess_);
  renderer_->end_frame();

  // Must open on this very first render -- no extra frame/input needed.
  EXPECT_TRUE(win2.get_as<bool>("__modal_opened__"_key, false));
  std::string label = "Second###" + bdg::wish::stable_id(win2);
  auto* w = ImGui::FindWindowByName(label.c_str());
  ASSERT_NE(w, nullptr);
  EXPECT_TRUE(w->Flags & ImGuiWindowFlags_Modal);
}

// ── Construction-time extra_render_fns dispatch ──────────────────────────────
//
// imgui_renderer's dispatch table is seeded with wish's built-ins at
// construction and merged with whatever the caller passes as
// extra_render_fns -- see imgui_renderer::imgui_renderer(). These functions
// must be plain function pointers (render_fn's signature), so state is
// tracked via file-scope statics rather than captures.

namespace {
bool g_dummy_render_called = false;

void render_dummy_test_element(bdg::wish::imgui_renderer&, const ui_element&, const context&) {
  g_dummy_render_called = true;
}
} // namespace

TEST_F(ImguiRendererTest, ExtraRenderFnsDispatchesToProjectSpecificClass) {
  g_dummy_render_called = false;
  auto map = bdg::wish::import_json(R"({"type":"Label","text":"x"})");
  auto& node = *map[""];
  node[dynamic::CLASS] = "DummyTestElement"_key;

  imgui_renderer r({{"DummyTestElement"_key.id, render_dummy_test_element}});
  r.begin_frame();
  in_window([&] { r.render_node(node, *sess_); });
  r.end_frame();

  EXPECT_TRUE(g_dummy_render_called);
}

TEST_F(ImguiRendererTest, ExtraRenderFnsDoNotClobberUnrelatedBuiltIns) {
  g_dummy_render_called = false;
  auto map = bdg::wish::import_json(R"({"type":"Label","text":"built-in"})");

  // Constructed with an extra entry for an unrelated class id -- the "Label"
  // built-in must still dispatch to wish's own render_label, not the dummy.
  imgui_renderer r({{"DummyTestElement"_key.id, render_dummy_test_element}});
  EXPECT_NO_THROW({
    r.begin_frame();
    in_window([&] { r.render_node(*map[""], *sess_); });
    r.end_frame();
  });

  EXPECT_FALSE(g_dummy_render_called);
}

TEST_F(ImguiRendererTest, ExtraRenderFnsOverrideIsPerInstanceNotGlobal) {
  g_dummy_render_called = false;
  auto map = bdg::wish::import_json(R"({"type":"Label","text":"overridden"})");

  // One instance overrides "Label"'s built-in dispatch entry...
  imgui_renderer overriding_r({{"Label"_key.id, render_dummy_test_element}});
  overriding_r.begin_frame();
  in_window([&] { overriding_r.render_node(*map[""], *sess_); });
  overriding_r.end_frame();
  EXPECT_TRUE(g_dummy_render_called);

  // ...but a separately-constructed instance with no override still uses
  // wish's built-in render_label, proving the mapping is per-instance.
  g_dummy_render_called = false;
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    in_window([&] { renderer_->render_node(*map[""], *sess_); });
    renderer_->end_frame();
  });
  EXPECT_FALSE(g_dummy_render_called);
}

// ── Offscreen render target (base class) ─────────────────────────────────────

// The base imgui_renderer has no GPU backend attached, so begin_render_target()
// must report "unsupported" via a null handle rather than throwing or
// fabricating a texture.
TEST_F(ImguiRendererTest, BaseBeginRenderTargetReturnsNullAndDoesNotThrow) {
  ImTextureID tex{};
  EXPECT_NO_THROW(tex = renderer_->begin_render_target(64, 64));
  EXPECT_EQ(tex, ImTextureID{});
}

// Calling end_render_target() without a preceding begin_render_target() call
// is documented as a safe no-op -- the base implementation has no state to
// restore.
TEST_F(ImguiRendererTest, BaseEndRenderTargetWithoutBeginIsSafeNoOp) {
  EXPECT_NO_THROW(renderer_->end_render_target());
}

// The base imgui_renderer has no backend to submit to, so flush_draw_list()
// must be a safe no-op rather than throwing or dereferencing a null backend.
TEST_F(ImguiRendererTest, BaseFlushDrawListIsSafeNoOp) {
  ImDrawList draw_list(ImGui::GetDrawListSharedData());
  draw_list._ResetForNewFrame();
  draw_list.AddRectFilled(ImVec2(0, 0), ImVec2(4, 4), IM_COL32(255, 0, 0, 255));
  EXPECT_NO_THROW(renderer_->flush_draw_list(draw_list, 4, 4));
}
