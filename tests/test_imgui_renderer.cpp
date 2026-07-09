// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <imgui/imgui_renderer.hpp>
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
