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

#include <cmath>
#include <memory>
#include <string>
#include <utility>
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
      // Like the server: docking containers first, so their layout exists
      // before the windows that target it submit.
      auto is_dock = [](const wish::ui_element_ptr& root) {
        return root->class_key() == bdg::bison::key_t{"DockSpaceViewport"};
      };
      // Like the server, each top-level object renders with its key as
      // current_top_level_key.
      for (auto& [key, root] : s.top_level_objects)
        if (root && is_dock(root)) {
          s.current_top_level_key = key;
          renderer_->render_session(*root, s);
        }
      for (auto& [key, root] : s.top_level_objects)
        if (root && !is_dock(root) && !wish::is_host_chrome_class(root->class_key())) {
          s.current_top_level_key = key;
          renderer_->render_session(*root, s);
        }
      s.current_top_level_key = bdg::bison::key_t{};
      for (auto& ev : s.pending_events)
        events_.push_back({ev.id, ev.event_name});
      s.pending_events.clear();
    }
    renderer_->end_frame();
    return desktop;
  }

  /// Clicks the left mouse button at @p pos: a frame to hover, one to
  /// press, one to release.
  void click(ImVec2 pos) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(pos.x, pos.y);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
  }

  /// Center of caption button @p index (0 = close, 1 = maximize,
  /// 2 = minimize, counted from the right) of @p win.
  static ImVec2 caption_button(const ImGuiWindow* win, int index) {
    const float h = win->TitleBarHeight;
    const float w = std::floor(h * 1.5f);
    return ImVec2(win->Pos.x + win->Size.x - w * (float(index) + 0.5f), win->Pos.y + h * 0.5f);
  }

  /// Whether a frame queued @p event from the element with `__wish_id` @p id.
  bool saw_event(hash_t id, const char* event) const {
    for (const auto& [ev_id, name] : events_)
      if (ev_id.id == id && name == bdg::bison::key_t{event})
        return true;
    return false;
  }

  static const char* kShell;

  std::vector<std::pair<bdg::bison::key_t, bdg::bison::key_t>> events_;
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

// ── Wallpaper layout ─────────────────────────────────────────────────────────

namespace {

std::vector<wish::detail::desktop_wallpaper_quad> layout(const std::string& fit, ImVec2 image) {
  // A 200 x 100 screen at (10, 20).
  return wish::detail::desktop_wallpaper_layout(fit, ImVec2(10, 20), ImVec2(200, 100), image);
}

void expect_quad(const wish::detail::desktop_wallpaper_quad& q, ImVec2 p0, ImVec2 p1, ImVec2 uv0, ImVec2 uv1) {
  EXPECT_FLOAT_EQ(q.p0.x, p0.x);
  EXPECT_FLOAT_EQ(q.p0.y, p0.y);
  EXPECT_FLOAT_EQ(q.p1.x, p1.x);
  EXPECT_FLOAT_EQ(q.p1.y, p1.y);
  EXPECT_FLOAT_EQ(q.uv0.x, uv0.x);
  EXPECT_FLOAT_EQ(q.uv0.y, uv0.y);
  EXPECT_FLOAT_EQ(q.uv1.x, uv1.x);
  EXPECT_FLOAT_EQ(q.uv1.y, uv1.y);
}

} // namespace

TEST(DesktopShellHelpers, WallpaperCoverCropsTheOverflowingAxis) {
  // A square image on a 2:1 screen: full width, the middle half of its height.
  auto q = layout("cover", ImVec2(50, 50));
  ASSERT_EQ(q.size(), size_t{1});
  expect_quad(q[0], ImVec2(10, 20), ImVec2(210, 120), ImVec2(0, 0.25f), ImVec2(1, 0.75f));
  // Unknown fits are cover.
  auto u = layout("zoom", ImVec2(50, 50));
  ASSERT_EQ(u.size(), size_t{1});
  expect_quad(u[0], ImVec2(10, 20), ImVec2(210, 120), ImVec2(0, 0.25f), ImVec2(1, 0.75f));
}

TEST(DesktopShellHelpers, WallpaperContainLetterboxes) {
  auto q = layout("contain", ImVec2(50, 50)); // scaled to 100 x 100, centered
  ASSERT_EQ(q.size(), size_t{1});
  expect_quad(q[0], ImVec2(60, 20), ImVec2(160, 120), ImVec2(0, 0), ImVec2(1, 1));
}

TEST(DesktopShellHelpers, WallpaperStretchFillsTheScreen) {
  auto q = layout("stretch", ImVec2(50, 50));
  ASSERT_EQ(q.size(), size_t{1});
  expect_quad(q[0], ImVec2(10, 20), ImVec2(210, 120), ImVec2(0, 0), ImVec2(1, 1));
  // An unknown image size stretches whatever the fit.
  auto u = layout("center", ImVec2(0, 0));
  ASSERT_EQ(u.size(), size_t{1});
  expect_quad(u[0], ImVec2(10, 20), ImVec2(210, 120), ImVec2(0, 0), ImVec2(1, 1));
}

TEST(DesktopShellHelpers, WallpaperCenterKeepsTheSizeAndCrops) {
  auto small = layout("center", ImVec2(50, 40));
  ASSERT_EQ(small.size(), size_t{1});
  expect_quad(small[0], ImVec2(85, 50), ImVec2(135, 90), ImVec2(0, 0), ImVec2(1, 1));
  auto wide = layout("center", ImVec2(400, 50)); // wider than the screen
  ASSERT_EQ(wide.size(), size_t{1});
  expect_quad(wide[0], ImVec2(10, 45), ImVec2(210, 95), ImVec2(0.25f, 0), ImVec2(0.75f, 1));
}

TEST(DesktopShellHelpers, WallpaperTileRepeatsAndCropsTheEdges) {
  auto q = layout("tile", ImVec2(80, 60)); // 3 columns x 2 rows
  ASSERT_EQ(q.size(), size_t{6});
  expect_quad(q[0], ImVec2(10, 20), ImVec2(90, 80), ImVec2(0, 0), ImVec2(1, 1));
  // Last column: 40 of 80 px shown; last row: 40 of 60 px.
  expect_quad(q[5], ImVec2(170, 80), ImVec2(210, 120), ImVec2(0, 0), ImVec2(0.5f, 40.0f / 60.0f));
  // A tiny image is capped at 64 x 64 tiles.
  EXPECT_LE(layout("tile", ImVec2(1, 1)).size(), size_t{64 * 64});
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

TEST_F(DesktopShellTest, ShutdownFieldRunsTheCallbackOncePerRequest) {
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
  EXPECT_FALSE(shell->get_as<bool>("shutdown"_key, true)); // reset once seen
  // A second request (after the confirmation was cancelled) runs it again.
  (*shell)["shutdown"_key] = true;
  frame_with();
  EXPECT_EQ(calls, 2);
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

TEST_F(DesktopShellTest, InstancesOfOneAppGetTheirOwnDockspace) {
  add_top(kShell, 1u);
  add_top(R"({"type":"DockSpaceViewport","id":"main","title":"Editor"})", 600u, 60u);
  add_top(R"({"type":"DockSpaceViewport","id":"main","title":"Editor"})", 601u, 61u);
  add_top(R"({"type":"Window","title":"P","dock_target":"main"})", 602u, 60u);
  add_top(R"({"type":"Window","title":"P","dock_target":"main"})", 603u, 61u);
  // Calling ImGui::DockSpace() twice with one ID asserts inside imgui.
  frame();
  frame();
  frame();

  ImGuiWindow* first = ImGui::FindWindowByName("Editor###main");
  ImGuiWindow* second = ImGui::FindWindowByName("Editor###main~1");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first->ID, second->ID);
  EXPECT_TRUE(first->Active);
  EXPECT_TRUE(second->Active);
}

TEST_F(DesktopShellTest, SecondInstanceDocksItsOwnPanels) {
  add_top(kShell, 1u);
  const char* vp = R"({"type":"DockSpaceViewport","id":"main","title":"Editor","children":{
    "lay":{"type":"DockLayout","target":"main","version":1,"children":{
      "s":{"type":"DockSplit","dir":"up","ratio":0.3,"children":{
        "a":{"type":"DockArea","windows":"__p0"},"b":{"type":"DockArea"}}}}}}})";
  const char* panel = R"({"type":"Window","title":"Panel","__path__":"__p0","dock_target":"main"})";
  for (auto [vid, pid, group] : {std::tuple{600u, 602u, 60u}, std::tuple{601u, 603u, 61u}}) {
    add_top(vp, vid, group);
    auto w = add_top(panel, pid, group);
    (*w)["__path__"_key] = std::string{"__p0"};
  }
  for (int i = 0; i < 5; ++i)
    frame();

  ImGuiWindow* v2 = ImGui::FindWindowByName("Editor###main~1");
  ImGuiWindow* p2 = ImGui::FindWindowByName("Panel###__p0~1");
  ASSERT_NE(v2, nullptr);
  ASSERT_NE(p2, nullptr);
  ASSERT_NE(p2->DockNode, nullptr);
  EXPECT_EQ(p2->DockNode->HostWindow->ParentWindow, v2); // the dockspace child of its own instance
  ImGuiWindow* p1 = ImGui::FindWindowByName("Panel###__p0");
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p1->DockNode, nullptr);
  EXPECT_NE(p1->DockNode->HostWindow->ParentWindow, v2);
}

// ── Caption (title-bar buttons) ──────────────────────────────────────────────

TEST_F(DesktopShellTest, MainWindowDrawsItsOwnTitleAndPanelsKeepImGuis) {
  add_top(kShell, 1u);
  add_top(R"({"type":"Window","title":"Main","width":400,"height":300})", 700u);
  add_top(R"({"type":"DockSpaceViewport","id":"app","title":"App","closable":true})", 701u, 70u);
  add_top(R"({"type":"Window","title":"Panel","dock_target":"app"})", 702u, 70u);
  frame();
  frame();

  // An app's main window hides ImGui's title: the caption draws it, before
  // the buttons. A panel keeps ImGui's title bar.
  ImGuiWindow* main = ImGui::FindWindowByName("###700");
  ImGuiWindow* app = ImGui::FindWindowByName("###app");
  ImGuiWindow* panel = ImGui::FindWindowByName("Panel###702");
  ASSERT_NE(main, nullptr);
  ASSERT_NE(app, nullptr);
  ASSERT_NE(panel, nullptr);
  EXPECT_STREQ(main->Name, "###700");
  EXPECT_STREQ(app->Name, "###app");
  EXPECT_STREQ(panel->Name, "Panel###702");
  EXPECT_FALSE(main->HasCloseButton); // the caption's own close button replaces it
  EXPECT_TRUE(main->Flags & ImGuiWindowFlags_NoCollapse);
}

TEST_F(DesktopShellTest, CaptionMinimizeHidesTheWindow) {
  add_top(kShell, 1u);
  auto w = add_top(R"({"type":"Window","title":"Main","width":400,"height":300})", 710u);
  frame();
  frame();
  ImGuiWindow* win = ImGui::FindWindowByName("###710");
  ASSERT_NE(win, nullptr);

  click(caption_button(win, 2));
  EXPECT_TRUE(static_cast<const wish::window&>(*w).minimized());
  frame(); // the click's own frame still submitted it
  EXPECT_FALSE(win->Active);
}

TEST_F(DesktopShellTest, CaptionMaximizeFillsTheDesktopAndRestores) {
  add_top(kShell, 1u);
  auto w = add_top(R"({"type":"Window","title":"Main","width":400,"height":300})", 720u);
  frame();
  frame();
  ImGuiWindow* win = ImGui::FindWindowByName("###720");
  ASSERT_NE(win, nullptr);
  const ImVec2 pos = win->Pos;
  const ImVec2 size = win->Size;

  click(caption_button(win, 1));
  frame();
  EXPECT_TRUE(static_cast<const wish::window&>(*w).maximize_state().maximized);
  const ImVec2 min = renderer_->desktop_area_min();
  const ImVec2 max = renderer_->desktop_area_max();
  EXPECT_FLOAT_EQ(win->Pos.x, min.x);
  EXPECT_FLOAT_EQ(win->Pos.y, min.y);
  EXPECT_FLOAT_EQ(win->Size.x, max.x - min.x);
  EXPECT_FLOAT_EQ(win->Size.y, max.y - min.y);
  EXPECT_TRUE(win->Flags & ImGuiWindowFlags_NoMove); // a maximized window stays put

  click(caption_button(win, 1));
  frame();
  EXPECT_FALSE(static_cast<const wish::window&>(*w).maximize_state().maximized);
  EXPECT_FLOAT_EQ(win->Pos.x, pos.x);
  EXPECT_FLOAT_EQ(win->Pos.y, pos.y);
  EXPECT_FLOAT_EQ(win->Size.x, size.x);
  EXPECT_FLOAT_EQ(win->Size.y, size.y);
}

TEST_F(DesktopShellTest, DoubleClickOnTheTitleMaximizes) {
  add_top(kShell, 1u);
  auto w = add_top(R"({"type":"Window","title":"Main","width":400,"height":300})", 730u);
  frame();
  frame();
  ImGuiWindow* win = ImGui::FindWindowByName("###730");
  ASSERT_NE(win, nullptr);

  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(win->Pos.x + 40.0f, win->Pos.y + win->TitleBarHeight * 0.5f);
  frame();
  for (bool down : {true, false, true, false}) {
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    frame();
  }
  EXPECT_TRUE(static_cast<const wish::window&>(*w).maximize_state().maximized);
}

TEST_F(DesktopShellTest, NonResizableWindowCannotMaximize) {
  add_top(kShell, 1u);
  auto w = add_top(R"({"type":"Window","title":"Calc","width":300,"height":200,"flags":"NoResize"})", 740u);
  frame();
  frame();
  ImGuiWindow* win = ImGui::FindWindowByName("###740");
  ASSERT_NE(win, nullptr);

  click(caption_button(win, 1));
  EXPECT_FALSE(static_cast<const wish::window&>(*w).maximize_state().maximized);
}

TEST_F(DesktopShellTest, CaptionCloseEmitsClosedOnlyWhenClosable) {
  add_top(kShell, 1u);
  add_top(R"({"type":"Window","title":"Fixed","width":300,"height":200,"pos_x":40,"pos_y":40})", 750u);
  add_top(R"({"type":"Window","title":"Doc","width":300,"height":200,"closable":true,"pos_x":500,"pos_y":40})",
      751u);
  frame();
  frame();
  ImGuiWindow* fixed = ImGui::FindWindowByName("###750");
  ImGuiWindow* doc = ImGui::FindWindowByName("###751");
  ASSERT_NE(fixed, nullptr);
  ASSERT_NE(doc, nullptr);

  click(caption_button(fixed, 0));
  EXPECT_FALSE(saw_event(750u, "closed"));
  click(caption_button(doc, 0));
  EXPECT_TRUE(saw_event(751u, "closed"));
}

TEST_F(DesktopShellTest, FrameCaptionMinimizesTheWholeAppAndCloses) {
  add_top(kShell, 1u);
  auto vp = add_top(R"({"type":"DockSpaceViewport","id":"app","title":"App","closable":true})", 760u, 76u);
  auto panel = add_top(R"({"type":"Window","title":"Panel","dock_target":"app"})", 761u, 76u);
  frame();
  frame();
  ImGuiWindow* win = ImGui::FindWindowByName("###app");
  ASSERT_NE(win, nullptr);

  click(caption_button(win, 0));
  EXPECT_TRUE(saw_event(760u, "closed"));

  click(caption_button(win, 2));
  EXPECT_TRUE(static_cast<const wish::ui_dockspace_viewport&>(*vp).minimized());
  EXPECT_TRUE(static_cast<const wish::window&>(*panel).minimized());
}

TEST_F(DesktopShellTest, ClosableFrameCanBeClosedFromTheTaskbar) {
  add_top(R"({"type":"DockSpaceViewport","id":"a","title":"A","closable":true})", 770u, 77u);
  add_top(R"({"type":"DockSpaceViewport","id":"b","title":"B"})", 771u, 78u);
  auto wins = wish::detail::desktop_collect_windows(sess(), session_.get(), {});
  ASSERT_EQ(wins.size(), size_t{2});
  std::sort(wins.begin(), wins.end(), [](const auto& a, const auto& b) { return a.top_key.id < b.top_key.id; });
  EXPECT_TRUE(wins[0].closable);
  EXPECT_FALSE(wins[1].closable);
}

TEST_F(DesktopShellTest, ClosableEmbeddedFrameOutsideADesktopUsesImGuisCloseButton) {
  add_top(R"({"type":"DockSpaceViewport","id":"app","title":"App","embedded":true,"closable":true})", 780u);
  frame();
  ImGuiWindow* win = ImGui::FindWindowByName("App###app");
  ASSERT_NE(win, nullptr);
  EXPECT_TRUE(win->HasCloseButton);
}

// ── Instance names stay stable ───────────────────────────────────────────────
//
// Several instances of one tool share a DockSpaceViewport `id`; each one's
// ImGui window names carry an instance suffix. That suffix must not change
// while the instance is open, or its frame, dockspace and panels become new
// ImGui windows and the panels fall out of the frame (the "panels all
// detached" layout corruption).

namespace {

const char* kInstanceViewport = R"({"type":"DockSpaceViewport","id":"main","title":"Editor","children":{
  "lay":{"type":"DockLayout","target":"main","version":1,"children":{
    "s":{"type":"DockSplit","dir":"up","ratio":0.3,"children":{
      "a":{"type":"DockArea","windows":"__p0"},"b":{"type":"DockArea"}}}}}}})";

} // namespace

TEST_F(DesktopShellTest, OpeningAnotherInstanceDoesNotRenameAnOpenOne) {
  add_top(kShell, 1u);
  // The open instance has the higher object id: an order-by-id suffix
  // would hand it a suffix as soon as the lower-id instance appears.
  add_top(kInstanceViewport, 901u, 91u);
  auto p = add_top(R"({"type":"Window","title":"Panel","dock_target":"main"})", 902u, 91u);
  (*p)["__path__"_key] = std::string{"__p0"};
  for (int i = 0; i < 4; ++i)
    frame();
  ImGuiWindow* panel = ImGui::FindWindowByName("Panel###__p0");
  ASSERT_NE(panel, nullptr);
  ASSERT_NE(panel->DockNode, nullptr);
  const ImGuiID frame_id = panel->DockNode->HostWindow->ParentWindow->ID;

  add_top(kInstanceViewport, 800u, 80u);
  auto q = add_top(R"({"type":"Window","title":"Panel","dock_target":"main"})", 802u, 80u);
  (*q)["__path__"_key] = std::string{"__p1"};
  for (int i = 0; i < 4; ++i)
    frame();

  EXPECT_TRUE(panel->Active); // same ImGui window, still submitted
  ASSERT_NE(panel->DockNode, nullptr);
  EXPECT_EQ(panel->DockNode->HostWindow->ParentWindow->ID, frame_id);
}

TEST_F(DesktopShellTest, ClosingAnInstanceDoesNotRenameTheOthers) {
  add_top(kShell, 1u);
  add_top(kInstanceViewport, 800u, 80u);
  auto p = add_top(R"({"type":"Window","title":"Panel","dock_target":"main"})", 802u, 80u);
  (*p)["__path__"_key] = std::string{"__p0"};
  for (int i = 0; i < 3; ++i)
    frame();
  add_top(kInstanceViewport, 901u, 91u);
  auto q = add_top(R"({"type":"Window","title":"Panel","dock_target":"main"})", 902u, 91u);
  (*q)["__path__"_key] = std::string{"__p1"};
  for (int i = 0; i < 4; ++i)
    frame();
  ImGuiWindow* second = ImGui::FindWindowByName("Panel###__p1~1");
  ASSERT_NE(second, nullptr);
  ASSERT_NE(second->DockNode, nullptr);
  const ImGuiID frame_id = second->DockNode->HostWindow->ParentWindow->ID;

  // The first instance goes away.
  {
    wish::context& s = sess();
    s.top_level_objects.erase(bdg::bison::key_t{800u});
    s.top_level_objects.erase(bdg::bison::key_t{802u});
    s.groups.erase(80u);
  }
  for (int i = 0; i < 4; ++i)
    frame();

  EXPECT_TRUE(second->Active);
  ASSERT_NE(second->DockNode, nullptr);
  EXPECT_EQ(second->DockNode->HostWindow->ParentWindow->ID, frame_id);
}

// ── Reset layout ─────────────────────────────────────────────────────────────

namespace {

// A frame with a DockLayout placing "__p0", and a document window docked by
// dock_target (not in the layout).
const char* kResetViewport = R"({"type":"DockSpaceViewport","id":"main","title":"Editor","children":{
  "lay":{"type":"DockLayout","target":"main","version":1,"children":{
    "s":{"type":"DockSplit","dir":"left","ratio":0.3,"children":{
      "a":{"type":"DockArea","windows":"__p0"},"b":{"type":"DockArea"}}}}}}})";

// Undocks @p name as a user dragging its tab out would.
void undock(const char* name) {
  ImGuiWindow* w = ImGui::FindWindowByName(name);
  ASSERT_NE(w, nullptr);
  ImGui::DockBuilderDockWindow(name, 0);
}

} // namespace

TEST_F(DesktopShellTest, ResetLayoutRedocksPanelsDraggedOut) {
  add_top(kShell, 1u);
  auto vp = add_top(kResetViewport, 950u, 95u);
  auto p = add_top(R"({"type":"Window","title":"Panel","dock_target":"main"})", 951u, 95u);
  (*p)["__path__"_key] = std::string{"__p0"};
  auto doc = add_top(R"({"type":"Window","title":"Doc","dock_target":"main"})", 952u, 95u);
  (*doc)["__path__"_key] = std::string{"__doc"};
  for (int i = 0; i < 4; ++i)
    frame();
  ImGuiWindow* panel = ImGui::FindWindowByName("Panel###__p0");
  ImGuiWindow* document = ImGui::FindWindowByName("Doc###__doc");
  ASSERT_NE(panel, nullptr);
  ASSERT_NE(document, nullptr);
  ASSERT_NE(panel->DockNode, nullptr);
  ASSERT_NE(document->DockNode, nullptr);

  undock("Panel###__p0");
  undock("Doc###__doc");
  for (int i = 0; i < 3; ++i)
    frame();
  ASSERT_EQ(panel->DockNode, nullptr);
  ASSERT_EQ(document->DockNode, nullptr);

  static_cast<const wish::ui_dockspace_viewport&>(*vp).request_layout_reset();
  for (int i = 0; i < 3; ++i)
    frame();
  ASSERT_NE(panel->DockNode, nullptr);
  ASSERT_NE(document->DockNode, nullptr);
  EXPECT_FALSE(panel->DockNode->IsCentralNode()); // its layout area again
  EXPECT_TRUE(document->DockNode->IsCentralNode()); // the documents area
  EXPECT_NE(document->DockNode, panel->DockNode);
  ImGuiWindow* frame_win = ImGui::FindWindowByName("###main");
  ASSERT_NE(frame_win, nullptr);
  EXPECT_EQ(panel->DockNode->HostWindow->ParentWindow, frame_win);
  EXPECT_EQ(document->DockNode->HostWindow->ParentWindow, frame_win);
}

TEST_F(DesktopShellTest, ResetLayoutLeavesDialogsAlone) {
  add_top(kShell, 1u);
  auto vp = add_top(kResetViewport, 960u, 96u);
  auto p = add_top(R"({"type":"Window","title":"Panel","dock_target":"main"})", 961u, 96u);
  (*p)["__path__"_key] = std::string{"__p0"};
  add_top(R"({"type":"Window","title":"Dialog","dock_target":"main","flags":"NoDocking","pos_x":50,"pos_y":50})",
      962u, 96u);
  for (int i = 0; i < 3; ++i)
    frame();
  static_cast<const wish::ui_dockspace_viewport&>(*vp).request_layout_reset();
  for (int i = 0; i < 3; ++i)
    frame();
  ImGuiWindow* dialog = ImGui::FindWindowByName("Dialog###962");
  ASSERT_NE(dialog, nullptr);
  EXPECT_EQ(dialog->DockNode, nullptr);
}

TEST_F(DesktopShellTest, LayoutResetRequestFieldResetsTheLayout) {
  add_top(kShell, 1u);
  auto vp = add_top(kResetViewport, 970u, 97u);
  auto p = add_top(R"({"type":"Window","title":"Panel","dock_target":"main"})", 971u, 97u);
  (*p)["__path__"_key] = std::string{"__p0"};
  for (int i = 0; i < 4; ++i)
    frame();
  undock("Panel###__p0");
  for (int i = 0; i < 3; ++i)
    frame();
  ImGuiWindow* panel = ImGui::FindWindowByName("Panel###__p0");
  ASSERT_NE(panel, nullptr);
  ASSERT_EQ(panel->DockNode, nullptr);

  (*vp)["layout_reset_request"_key] = int32_t{1}; // a client bumps the field
  for (int i = 0; i < 3; ++i)
    frame();
  EXPECT_NE(panel->DockNode, nullptr);
}

TEST_F(DesktopShellTest, RightClickOnTheTitleOpensTheWindowMenu) {
  add_top(kShell, 1u);
  add_top(R"({"type":"Window","title":"Main","width":400,"height":300})", 980u);
  frame();
  frame();
  ImGuiWindow* win = ImGui::FindWindowByName("###980");
  ASSERT_NE(win, nullptr);

  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(win->Pos.x + 40.0f, win->Pos.y + win->TitleBarHeight * 0.5f);
  frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
  frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
  frame();
  ASSERT_FALSE(GImGui->OpenPopupStack.empty());
  EXPECT_EQ(GImGui->OpenPopupStack.back().PopupId, win->GetID("##wish_caption_menu"));
}

// ── Several sessions on one server ───────────────────────────────────────────

TEST_F(DesktopShellTest, SameToolInTwoSessionsGetsDistinctWindows) {
  // No desktop: two clients each running the same tool against one server.
  auto other =
      std::make_shared<wish::sync_context>(std::make_unique<wish::context>(bdg::bison::rmi::shared::generate_id()));
  const char* vp = R"({"type":"DockSpaceViewport","id":"main","title":"Editor","embedded":true})";
  for (auto* sc : {&session_, &other}) {
    auto lock = (*sc)->wlock();
    auto& s = static_cast<wish::context&>(**lock);
    auto root = wish::import_json(vp)[""];
    (*root)["__wish_id"_key] = bdg::bison::key_t{sc == &session_ ? 990u : 991u};
    s.top_level_objects[bdg::bison::key_t{1u}] = root;
  }
  // Calling ImGui::DockSpace() twice with one ID asserts inside imgui.
  for (int i = 0; i < 3; ++i) {
    renderer_->begin_frame();
    for (auto* sc : {&session_, &other}) {
      auto lock = (*sc)->wlock();
      auto& s = static_cast<wish::context&>(**lock);
      s.current_top_level_key = bdg::bison::key_t{1u};
      renderer_->render_session(*s.top_level_objects.at(bdg::bison::key_t{1u}), s);
      s.pending_events.clear();
    }
    renderer_->end_frame();
  }
  ImGuiWindow* first = ImGui::FindWindowByName("Editor###main");
  ImGuiWindow* second = ImGui::FindWindowByName("Editor###main~1");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_TRUE(first->Active);
  EXPECT_TRUE(second->Active);
}
