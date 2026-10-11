// MIT License © 2026 Binary Dice Games
// Tests for src/imgui/imgui_desktop_shell.hpp -- the desktop chrome drawn for
// a DesktopShell session -- and the desktop-mode window handling it turns on
// in render_window()/render_dockspace_viewport() (cascade, minimize).
#include <gtest/gtest.h>

#include <context/context.hpp>
#include <imgui/imgui_desktop_shell.hpp>
#include <imgui/imgui_renderer.hpp>
#include <server/registry.hpp>
#include <server/renderer.hpp>
#include <ui/ui_elements/window.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <memory>
#include <string>
#include <vector>

using namespace bdg::bison;
namespace wish = bdg::wish;

namespace {

class DesktopShellTest : public ::testing::Test {
 protected:
  void SetUp() override {
    wish::register_all();
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // hermetic: no imgui.ini
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    io.Fonts->SetTexID(ImTextureID{1});
    io.MousePos = ImVec2(-100.0f, -100.0f);
    session_ =
        std::make_shared<wish::sync_context>(std::make_unique<wish::context>(bdg::bison::rmi::shared::generate_id()));
    renderer_ = std::make_unique<wish::imgui_renderer>();
  }

  void TearDown() override {
    renderer_.reset();
    session_.reset();
    ImGui::DestroyContext(ctx_);
  }

  wish::context& sess() {
    return static_cast<wish::context&>(**session_->wlock());
  }

  /// Imports @p json as a top-level object of the session, with
  /// `__wish_id` @p id, filed under object group @p group (0: none).
  wish::ui_element_ptr add_top(const std::string& json, hash_t id, hash_t group = 0u) {
    auto tree = wish::import_json(json);
    auto root = tree[""];
    (*root)["__wish_id"_key] = bdg::bison::key_t{id};
    wish::context& s = sess();
    s.top_level_objects[bdg::bison::key_t{id}] = root;
    if (group != 0u)
      s.groups[group].insert(id);
    return root;
  }

  /// One frame: begin, the desktop chrome, the session's windows, end.
  bool frame() {
    renderer_->begin_frame();
    std::vector<wish::sync_context_ptr> sessions{session_};
    bool desktop = wish::render_desktop_shell(*renderer_, sessions, state_, [] {});
    {
      auto lock = session_->wlock();
      auto& s = static_cast<wish::context&>(**lock);
      for (auto& [key, root] : s.top_level_objects)
        if (root && !wish::is_host_chrome_class(root->class_key()))
          renderer_->render_session(*root, s);
      s.pending_events.clear();
    }
    renderer_->end_frame();
    return desktop;
  }

  static const char* kShell;

  ImGuiContext* ctx_ = nullptr;
  wish::sync_context_ptr session_;
  std::unique_ptr<wish::imgui_renderer> renderer_;
  wish::desktop_shell_state state_;
};

const char* DesktopShellTest::kShell = R"({"type":"DesktopShell","user":"tester","children":{
  "e0":{"type":"DesktopApp","id":"ed","app":"test/apps/editor","label":"editor","tooltip":"Edits text",
        "pinned":true,"on_desktop":true},
  "e1":{"type":"DesktopMenu","id":"tools","label":"Tools","children":{
    "e0":{"type":"DesktopApp","id":"calc","app":"test/apps/calc","label":"calc","tooltip":"Adds numbers"},
    "e1":{"type":"Separator"},
    "e2":{"type":"DesktopMenu","id":"tools/more","label":"More","children":{
      "e0":{"type":"DesktopApp","id":"dark","label":"Dark theme"}}}}},
  "e2":{"type":"Separator"},
  "power":{"type":"DesktopMenu","id":"power","role":"power","children":{
    "e0":{"type":"DesktopApp","id":"bye","label":"Quit desktop"}}}}})";

} // namespace

// ── Helpers ──────────────────────────────────────────────────────────────────

TEST(DesktopShellHelpers, DisplayTitleDropsTheIdSuffix) {
  EXPECT_EQ(wish::detail::desktop_display_title("Editor##doc1", "x"), "Editor");
  EXPECT_EQ(wish::detail::desktop_display_title("Editor###doc1", "x"), "Editor");
  EXPECT_EQ(wish::detail::desktop_display_title("##hidden", "fallback"), "fallback");
  EXPECT_EQ(wish::detail::desktop_display_title("", "fallback"), "fallback");
}

TEST(DesktopShellHelpers, SearchRanksNamePrefixFirst) {
  wish::desktop_app_info nano{.label = "nano", .tooltip = "Text editor"};
  wish::desktop_app_info du{.label = "du", .tooltip = "Disk usage analyzer"};
  EXPECT_EQ(wish::detail::desktop_app_match_rank(nano, "na"), 0);
  EXPECT_EQ(wish::detail::desktop_app_match_rank(du, "na"), 2); // "analyzer"
  EXPECT_EQ(wish::detail::desktop_app_match_rank(nano, "AN"), 1);
  EXPECT_EQ(wish::detail::desktop_app_match_rank(nano, "zzz"), -1);
  EXPECT_EQ(wish::detail::desktop_app_match_rank(nano, ""), 0);
  EXPECT_FALSE(wish::detail::desktop_app_matches(du, "zzz"));
}

TEST(DesktopShellHelpers, CalendarMath) {
  EXPECT_EQ(wish::detail::desktop_days_in_month(2024, 1), 29);
  EXPECT_EQ(wish::detail::desktop_days_in_month(2026, 1), 28);
  EXPECT_EQ(wish::detail::desktop_days_in_month(2026, 9), 31);
  EXPECT_EQ(wish::detail::desktop_days_in_month(2026, 10), 30);
  EXPECT_EQ(wish::detail::desktop_first_weekday(2026, 9), 3); // 1 Oct 2026: Thursday
  EXPECT_EQ(wish::detail::desktop_first_weekday(2024, 0), 0); // 1 Jan 2024: Monday
  EXPECT_EQ(wish::detail::desktop_first_weekday(2023, 9), 6); // 1 Oct 2023: Sunday
}

TEST(DesktopShellHelpers, FormatTime) {
  EXPECT_EQ(wish::detail::desktop_format_time("", 0), "");
  EXPECT_EQ(wish::detail::desktop_format_time("%Y", 86400 * 400).size(), size_t{4});
}

// ── Collecting apps and windows ──────────────────────────────────────────────

TEST_F(DesktopShellTest, CollectsTheMenuTree) {
  auto shell = add_top(kShell, 1u);
  auto menu = wish::detail::desktop_collect_menu(*shell);

  // Every entry, flat: the tree's, then the power menu's.
  ASSERT_EQ(menu.apps.size(), size_t{4});
  EXPECT_EQ(menu.apps[0].id, "ed");
  EXPECT_EQ(menu.apps[0].tooltip, "Edits text");
  EXPECT_TRUE(menu.apps[0].pinned);
  EXPECT_TRUE(menu.apps[0].on_desktop);
  EXPECT_EQ(menu.apps[1].id, "calc");
  EXPECT_EQ(menu.apps[1].path, "Tools");
  EXPECT_EQ(menu.apps[2].id, "dark");
  EXPECT_EQ(menu.apps[2].path, "Tools / More");
  EXPECT_FALSE(menu.apps[2].power);
  EXPECT_EQ(menu.apps[3].id, "bye");
  EXPECT_TRUE(menu.apps[3].power);

  // The tree: entry, folder (entry, separator, folder), separator.
  using kind = wish::desktop_menu_node::kind;
  ASSERT_EQ(menu.items.size(), size_t{3});
  EXPECT_EQ(menu.items[0].type, kind::entry);
  EXPECT_EQ(menu.items[0].app_index, 0);
  EXPECT_EQ(menu.items[1].type, kind::folder);
  EXPECT_EQ(menu.items[1].id, "tools");
  ASSERT_EQ(menu.items[1].children.size(), size_t{3});
  EXPECT_EQ(menu.items[1].children[1].type, kind::separator);
  EXPECT_EQ(menu.items[1].children[2].children[0].app_index, 2);
  EXPECT_EQ(menu.items[2].type, kind::separator);

  // The power role is not a folder of the list.
  ASSERT_EQ(menu.power.size(), size_t{1});
  EXPECT_EQ(menu.power[0].app_index, 3);
}

TEST_F(DesktopShellTest, CollectAppsFlattensTheTree) {
  auto shell = add_top(kShell, 1u);
  auto apps = wish::detail::desktop_collect_apps(*shell);
  ASSERT_EQ(apps.size(), size_t{4});
  EXPECT_EQ(apps[1].label, "calc");
  EXPECT_FALSE(apps[1].pinned);
}

TEST_F(DesktopShellTest, ShutdownFieldRunsTheCallbackOnce) {
  auto shell = add_top(kShell, 1u);
  int calls = 0;
  auto frame_with = [&] {
    renderer_->begin_frame();
    std::vector<wish::sync_context_ptr> sessions{session_};
    wish::render_desktop_shell(*renderer_, sessions, state_, [&] { ++calls; });
    renderer_->end_frame();
  };
  frame_with();
  EXPECT_EQ(calls, 0);
  (*shell)["shutdown"_key] = true;
  frame_with();
  frame_with();
  EXPECT_EQ(calls, 1);
}

TEST_F(DesktopShellTest, CollectsVisibleNonModalWindowsWithTheirApp) {
  add_top(R"({"type":"Window","title":"Notes##w1"})", 100u, 5u);
  add_top(R"({"type":"Window","title":"Dialog","modal":true})", 101u, 5u);
  add_top(R"({"type":"Window","title":"Hidden","visible":false})", 102u);
  add_top(R"({"type":"Label","text":"not a window"})", 103u);
  add_top(R"({"type":"Window","title":"Other"})", 104u);

  wish::desktop_app_info editor{.app = "test/apps/editor", .label = "editor", .sessions = {5}};
  auto wins = wish::detail::desktop_collect_windows(sess(), session_.get(), {editor});
  ASSERT_EQ(wins.size(), size_t{2});
  std::sort(wins.begin(), wins.end(), [](const auto& a, const auto& b) { return a.top_key.id < b.top_key.id; });
  EXPECT_EQ(wins[0].title, "Notes");
  EXPECT_EQ(wins[0].app_index, 0);
  EXPECT_FALSE(wins[0].frame);
  EXPECT_EQ(wins[1].title, "Other");
  EXPECT_EQ(wins[1].app_index, -1);
}

TEST_F(DesktopShellTest, UnlistedAppsAreCollectedAndOwnTheirWindows) {
  auto app = add_top(
      R"({"type":"DesktopApp","app":"ext/py/todo","label":"To-do","icon":"res/icons/check.png","listed":false})", 600u);
  (*app)["sessions"_key] = std::vector<int32_t>{9};
  add_top(R"({"type":"Window","title":"To-do"})", 601u, 9u);

  auto info = wish::detail::desktop_collect_app(*app);
  EXPECT_FALSE(info.listed);
  EXPECT_EQ(info.label, "To-do");

  auto wins = wish::detail::desktop_collect_windows(sess(), session_.get(), {info});
  ASSERT_EQ(wins.size(), size_t{1});
  EXPECT_EQ(wins[0].app_index, 0);
}

TEST_F(DesktopShellTest, TopLevelDesktopAppIsNotRenderedAsAWindow) {
  add_top(kShell, 1u);
  add_top(R"({"type":"DesktopApp","app":"ext/py/todo","label":"To-do","listed":false})", 610u);
  EXPECT_TRUE(frame());
  EXPECT_TRUE(wish::is_host_chrome_class(bdg::bison::key_t{"DesktopApp"}));
}

TEST_F(DesktopShellTest, AppFrameStandsForItsWindows) {
  add_top(R"({"type":"DockSpaceViewport","id":"main","title":"Editor"})", 200u, 7u);
  add_top(R"({"type":"Window","title":"Toolbar"})", 201u, 7u);
  add_top(R"({"type":"Window","title":"Unrelated"})", 202u, 8u);

  auto wins = wish::detail::desktop_collect_windows(sess(), session_.get(), {});
  ASSERT_EQ(wins.size(), size_t{2});
  auto frame = std::find_if(wins.begin(), wins.end(), [](const auto& w) { return w.frame; });
  ASSERT_NE(frame, wins.end());
  EXPECT_EQ(frame->title, "Editor");
  EXPECT_EQ(frame->members.size(), size_t{2});
}

// ── Chrome ───────────────────────────────────────────────────────────────────

TEST_F(DesktopShellTest, NoShellDrawsNothing) {
  add_top(R"({"type":"Window","title":"W"})", 300u);
  EXPECT_FALSE(frame());
  EXPECT_FALSE(renderer_->desktop_cascade_active());
  EXPECT_EQ(ImGui::FindWindowByName("##wish_taskbar"), nullptr);
}

TEST_F(DesktopShellTest, ShellDrawsTaskbarAndShrinksTheWorkArea) {
  add_top(kShell, 1u);
  EXPECT_TRUE(frame());
  EXPECT_TRUE(renderer_->desktop_cascade_active());
  ImGuiWindow* bar = ImGui::FindWindowByName("##wish_taskbar");
  ASSERT_NE(bar, nullptr);
  EXPECT_FLOAT_EQ(bar->Pos.y + bar->Size.y, 800.0f); // along the bottom edge
  frame();
  // Windows and the dockspace live above the taskbar.
  EXPECT_LT(ImGui::GetMainViewport()->WorkSize.y, 800.0f);
}

TEST_F(DesktopShellTest, NewWindowsFloatAtCascadingPositions) {
  add_top(kShell, 1u);
  add_top(R"({"type":"Window","title":"A","width":300,"height":200})", 400u);
  frame();
  add_top(R"({"type":"Window","title":"B","width":300,"height":200})", 401u);
  frame();
  frame();

  ImGuiWindow* a = ImGui::FindWindowByName("A###400");
  ImGuiWindow* b = ImGui::FindWindowByName("B###401");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->DockNode, nullptr); // floating, not docked into the host
  EXPECT_GT(b->Pos.x, a->Pos.x);
  EXPECT_GT(b->Pos.y, a->Pos.y);
}

TEST_F(DesktopShellTest, NoDockingWindowsCascadeToo) {
  add_top(kShell, 1u);
  add_top(R"({"type":"Window","title":"A","width":300,"height":200,"flags":"NoDocking"})", 420u);
  frame();
  add_top(R"({"type":"Window","title":"B","width":300,"height":200,"flags":"NoDocking"})", 421u);
  frame();
  frame();

  ImGuiWindow* a = ImGui::FindWindowByName("A###420");
  ImGuiWindow* b = ImGui::FindWindowByName("B###421");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_GT(b->Pos.x, a->Pos.x);
  EXPECT_GT(b->Pos.y, a->Pos.y);
}

TEST_F(DesktopShellTest, MinimizedWindowIsNotSubmitted) {
  add_top(kShell, 1u);
  auto w = add_top(R"({"type":"Window","title":"M"})", 500u);
  frame();
  ImGuiWindow* win = ImGui::FindWindowByName("M###500");
  ASSERT_NE(win, nullptr);
  EXPECT_TRUE(win->Active);

  static_cast<const wish::window&>(*w).set_minimized(true);
  frame();
  EXPECT_FALSE(win->Active);

  static_cast<const wish::window&>(*w).set_minimized(false);
  frame();
  EXPECT_TRUE(win->Active);
}
