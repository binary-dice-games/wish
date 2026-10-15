// MIT License © 2026 Binary Dice Games
// Tests for the server overlay (src/imgui/imgui_server_overlay.hpp), the
// toast notifications (src/server/notifications.hpp,
// src/imgui/imgui_notifications.hpp) and machine_info.
#include <gtest/gtest.h>

#include <context/context.hpp>
#include <context/persistent_store.hpp>
#include <imgui/imgui_notifications.hpp>
#include <imgui/imgui_renderer.hpp>
#include <imgui/imgui_server_overlay.hpp>
#include <server/machine_info.hpp>
#include <server/notifications.hpp>
#include <server/registry.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_common.hpp"
#include "src/rmi/shared/ids.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace bdg::bison;
namespace wish = bdg::wish;
using namespace std::chrono_literals;

namespace {

// A fresh directory under the system temp dir, removed on destruction.
struct temp_dir {
  std::filesystem::path path;
  temp_dir() {
    path = std::filesystem::temp_directory_path() /
        ("wish_overlay_" + std::string{::testing::UnitTest::GetInstance()->current_test_info()->name()} + "_" +
         std::to_string(bdg::bison::rmi::shared::generate_id().id));
    std::filesystem::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

wish::notification note(std::string title, std::chrono::milliseconds duration = 6s, std::string tag = {}) {
  wish::notification n;
  n.title = std::move(title);
  n.duration = duration;
  n.tag = std::move(tag);
  return n;
}

} // namespace

// ── notification_center ─────────────────────────────────────────────────────

TEST(NotificationCenter, ActiveListsPostedNotificationsOldestFirst) {
  wish::notification_center c;
  const uint64_t a = c.post(note("first"));
  const uint64_t b = c.post(note("second"));
  EXPECT_NE(a, b);
  auto items = c.active();
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0].content.title, "first");
  EXPECT_EQ(items[1].content.title, "second");
  EXPECT_EQ(items[1].id, b);
}

TEST(NotificationCenter, ExpiredNotificationsAreDropped) {
  wish::notification_center c;
  c.post(note("short", 1s));
  c.post(note("long", 10s));
  auto items = c.active(std::chrono::steady_clock::now() + 2s);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].content.title, "long");
  EXPECT_EQ(c.active().size(), 1u) << "the expired one is gone for good";
}

TEST(NotificationCenter, ZeroDurationIsSticky) {
  wish::notification_center c;
  const uint64_t id = c.post(note("sticky", 0ms));
  EXPECT_EQ(c.active(std::chrono::steady_clock::now() + 24h).size(), 1u);
  c.dismiss(id);
  EXPECT_TRUE(c.active().empty());
}

TEST(NotificationCenter, SameTagReplacesInsteadOfStacking) {
  wish::notification_center c;
  const uint64_t first = c.post(note("v1", 6s, "hint"));
  c.post(note("other"));
  const uint64_t second = c.post(note("v2", 6s, "hint"));
  EXPECT_EQ(first, second);
  auto items = c.active();
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0].content.title, "v2");
}

TEST(NotificationCenter, OldestDroppedBeyondMax) {
  wish::notification_center c;
  for (size_t i = 0; i < wish::notification_center::kMaxActive + 3; ++i)
    c.post(note("n" + std::to_string(i)));
  auto items = c.active();
  ASSERT_EQ(items.size(), wish::notification_center::kMaxActive);
  EXPECT_EQ(items.front().content.title, "n3");
}

TEST(NotificationCenter, ConsumeChangedReportsPostsAndDismissalsOnce) {
  wish::notification_center c;
  EXPECT_FALSE(c.consume_changed());
  const uint64_t id = c.post(note("x"));
  EXPECT_TRUE(c.consume_changed());
  EXPECT_FALSE(c.consume_changed());
  c.dismiss(id + 100); // unknown: no change
  EXPECT_FALSE(c.consume_changed());
  c.dismiss(id);
  EXPECT_TRUE(c.consume_changed());
}

TEST(NotificationDetail, SplitKeyCaps) {
  using V = std::vector<std::string>;
  EXPECT_EQ(wish::detail::split_key_caps("Shift+F1"), (V{"Shift", "F1"}));
  EXPECT_EQ(wish::detail::split_key_caps("Ctrl + Shift + O"), (V{"Ctrl", "Shift", "O"}));
  EXPECT_EQ(wish::detail::split_key_caps("Ctrl++"), (V{"Ctrl", "+"}));
  EXPECT_TRUE(wish::detail::split_key_caps("").empty());
}

TEST(NotificationDetail, AlphaFadesInAndOut) {
  EXPECT_FLOAT_EQ(wish::detail::notification_alpha(0.0f, 5.0f), 0.0f);
  EXPECT_FLOAT_EQ(wish::detail::notification_alpha(1.0f, 5.0f), 1.0f);
  EXPECT_NEAR(wish::detail::notification_alpha(1.0f, 0.25f), 0.5f, 1e-5f);
  EXPECT_FLOAT_EQ(wish::detail::notification_alpha(1.0f, -1.0f), 1.0f) << "sticky never fades out";
}

// ── machine_info ────────────────────────────────────────────────────────────

TEST(MachineInfo, FormatBytes) {
  EXPECT_EQ(wish::format_bytes(512), "512 B");
  EXPECT_EQ(wish::format_bytes(1536), "1.5 KB");
  EXPECT_EQ(wish::format_bytes(uint64_t{3} << 30), "3.0 GB");
}

TEST(MachineInfo, FormatDuration) {
  EXPECT_EQ(wish::format_duration(42s), "42s");
  EXPECT_EQ(wish::format_duration(303s), "5m 03s");
  EXPECT_EQ(wish::format_duration(2h + 7min), "2h 07m");
  EXPECT_EQ(wish::format_duration(76h), "3d 04h");
  EXPECT_EQ(wish::format_duration(-5s), "0s");
}

TEST(MachineInfo, QueryDescribesThisProcess) {
  const wish::machine_info m = wish::query_machine_info();
  EXPECT_GT(m.pid, 0u);
  EXPECT_GT(m.cpu_count, 0u);
  EXPECT_FALSE(m.os.empty());
}

// ── Overlay helpers ─────────────────────────────────────────────────────────

TEST(ServerOverlayDetail, ReadLogTailKeepsTheLastLines) {
  temp_dir dir;
  const auto file = dir.path / "server.log";
  {
    std::ofstream out(file, std::ios::binary);
    for (int i = 0; i < 10; ++i)
      out << "line " << i << "\r\n";
  }
  auto lines = wish::detail::read_log_tail(file, 3);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "line 7");
  EXPECT_EQ(lines[2], "line 9");
  EXPECT_TRUE(wish::detail::read_log_tail(dir.path / "missing.log", 3).empty());
}

TEST(ServerOverlayDetail, ListStoreIdentitiesMergesFilesAndConnectedSessions) {
  temp_dir dir;
  std::filesystem::create_directories(dir.path / "users");
  std::ofstream(dir.path / "users" / "alice.bison") << "";
  std::ofstream(dir.path / "users" / "bob.bison") << "";
  std::ofstream(dir.path / "users" / "notes.txt") << "";
  auto ids = wish::detail::list_store_identities(dir.path, {"carol", "", "../evil", "alice"});
  EXPECT_EQ(ids, (std::vector<std::string>{"alice", "bob", "carol"}));
}

TEST(ServerOverlayDetail, HexColorsRoundTrip) {
  auto c = wish::detail::parse_hex_color("#FF8000");
  ASSERT_TRUE(c);
  EXPECT_FLOAT_EQ((*c)[0], 1.0f);
  EXPECT_NEAR((*c)[1], 128.0f / 255.0f, 1e-6f);
  EXPECT_FLOAT_EQ((*c)[3], 1.0f);
  EXPECT_EQ(wish::detail::to_hex_color(c->data(), false), "#FF8000");
  auto a = wish::detail::parse_hex_color("#11223344");
  ASSERT_TRUE(a);
  EXPECT_EQ(wish::detail::to_hex_color(a->data(), true), "#11223344");
  EXPECT_FALSE(wish::detail::parse_hex_color("FF8000"));
  EXPECT_FALSE(wish::detail::parse_hex_color("#GG0000"));
}

TEST(ServerOverlayDetail, CollectClientsListsIdentityAndWindowTitles) {
  wish::register_all();
  auto sync = std::make_shared<wish::sync_context>(std::make_unique<wish::context>(rmi::shared::generate_id()));
  {
    auto lock = sync->wlock();
    auto& s = static_cast<wish::context&>(**lock);
    s.identity = "alice";
    auto add = [&](const std::string& json, hash_t id) {
      auto root = wish::import_json(json)[""];
      s.top_level_objects[bdg::bison::key_t{id}] = root;
    };
    add(R"({"type": "Window", "title": "Editor##3"})", 101);
    add(R"({"type": "DockSpaceViewport", "id": "frame"})", 102);
    add(R"({"type": "MenuBarExtension"})", 103);
  }
  auto clients = wish::collect_overlay_clients({sync});
  ASSERT_EQ(clients.size(), 1u);
  EXPECT_EQ(clients[0].identity, "alice");
  EXPECT_EQ(clients[0].windows, (std::vector<std::string>{"Editor", "frame"}));
}

// ── ImGui: overlay and toasts ───────────────────────────────────────────────

namespace {

class ServerOverlayTest : public ::testing::Test {
 protected:
  void SetUp() override {
    wish::register_all();
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    io.Fonts->SetTexID(ImTextureID{1});
    io.MousePos = ImVec2(-100.0f, -100.0f);
    renderer_ = std::make_unique<wish::imgui_renderer>();
    session_ = std::make_shared<wish::sync_context>(std::make_unique<wish::context>(rmi::shared::generate_id()));

    stores_ = std::make_shared<wish::store_registry>(dir_.path / "store");
    log_file_ = dir_.path / "server.log";
    std::ofstream(log_file_) << "[info] server started\n[error] something failed\n";
    notifications_ = std::make_shared<wish::notification_center>();
  }

  void TearDown() override {
    overlay_.reset();
    renderer_.reset();
    session_.reset();
    ImGui::DestroyContext(ctx_);
  }

  wish::server_overlay& make_overlay(std::string hotkey = "Shift+F1") {
    wish::server_overlay_info info;
    info.version = "1.0";
    info.hotkey = std::move(hotkey);
    info.endpoint = "tcp 0.0.0.0:7070";
    info.renderer = "sdl3";
    info.log_file = log_file_;
    info.stores = stores_;
    info.shutdown = [this] { ++shutdowns_; };
    info.open_in_shell = [this](const std::string& target) {
      opened_.push_back(target);
      return open_result_;
    };
    overlay_ = std::make_unique<wish::server_overlay>(std::move(info), notifications_);
    return *overlay_;
  }

  /// One frame like host_renderer's: keys, a client window, overlay, toasts.
  void frame() {
    renderer_->begin_frame();
    if (overlay_)
      overlay_->handle_keys();
    ImGui::SetNextWindowPos({100, 100}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({300, 200}, ImGuiCond_Once);
    if (focus_client_) {
      ImGui::SetNextWindowFocus();
      focus_client_ = false;
    }
    ImGui::Begin("Client window");
    ImGui::TextUnformatted("app");
    ImGui::End();
    if (overlay_)
      overlay_->render({session_});
    toasts_visible_ = wish::render_notifications(*notifications_);
    renderer_->end_frame();
  }

  void press(ImGuiKeyChord chord) {
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    if (chord & ImGuiMod_Shift)
      io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddKeyEvent(key, true);
    frame();
    io.AddKeyEvent(key, false);
    if (chord & ImGuiMod_Shift)
      io.AddKeyEvent(ImGuiMod_Shift, false);
    frame();
  }

  static int display_index(const char* name) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = 0; i < g.Windows.Size; ++i) {
      if (std::strcmp(g.Windows[i]->Name, name) == 0)
        return i;
    }
    return -1;
  }

  temp_dir dir_;
  ImGuiContext* ctx_{nullptr};
  std::unique_ptr<wish::imgui_renderer> renderer_;
  wish::sync_context_ptr session_;
  wish::store_registry_ptr stores_;
  std::filesystem::path log_file_;
  wish::notification_center_ptr notifications_;
  std::unique_ptr<wish::server_overlay> overlay_;
  std::vector<std::string> opened_;
  bool open_result_{true};
  int shutdowns_{0};
  bool focus_client_{false};
  bool toasts_visible_{false};
};

} // namespace

TEST_F(ServerOverlayTest, HotkeyTogglesAndEscapeCloses) {
  auto& ov = make_overlay();
  ASSERT_TRUE(ov.has_hotkey());
  frame();
  EXPECT_FALSE(ov.is_open());
  press(ImGuiMod_Shift | ImGuiKey_F1);
  EXPECT_TRUE(ov.is_open());
  EXPECT_NE(display_index("##wish_server_overlay"), -1);
  press(ImGuiMod_Shift | ImGuiKey_F1);
  EXPECT_FALSE(ov.is_open());
  press(ImGuiMod_Shift | ImGuiKey_F1);
  press(ImGuiKey_Escape);
  EXPECT_FALSE(ov.is_open());
}

TEST_F(ServerOverlayTest, InvalidHotkeyDisablesTheShortcut) {
  auto& ov = make_overlay("Shift+Nope");
  EXPECT_FALSE(ov.has_hotkey());
  press(ImGuiMod_Shift | ImGuiKey_F1);
  EXPECT_FALSE(ov.is_open());
  ov.set_open(true); // still reachable from the menu bar
  frame();
  EXPECT_TRUE(ov.is_open());
}

TEST_F(ServerOverlayTest, StaysAboveAClientWindowThatTakesFocus) {
  auto& ov = make_overlay();
  frame();
  ov.set_open(true);
  frame();
  frame();
  focus_client_ = true; // e.g. an app focusing one of its windows
  frame();
  frame();
  EXPECT_GT(display_index("##wish_server_overlay"), display_index("Client window"));
}

TEST_F(ServerOverlayTest, EveryPageRenders) {
  {
    // A described server setting, so the Settings page has a row.
    wish::settings::setting_info info;
    info.name = "wish.cli.server.font_size";
    info.label = "Text size";
    info.category = "appearance";
    info.type = wish::settings::setting_type::integer;
    info.default_value = int32_t{16};
    info.min = 8.0f;
    info.max = 48.0f;
    wish::settings::describe(*stores_->server_store(), info);
  }
  auto& ov = make_overlay();
  ov.set_open(true);
  for (auto p : {wish::server_overlay::page::overview, wish::server_overlay::page::clients,
                 wish::server_overlay::page::settings, wish::server_overlay::page::logs,
                 wish::server_overlay::page::about}) {
    ov.set_page(p);
    frame();
    frame();
    EXPECT_EQ(ov.current_page(), p);
  }
  EXPECT_TRUE(ov.is_open());
  EXPECT_EQ(shutdowns_, 0);
}

TEST_F(ServerOverlayTest, FailedOpenPostsAnErrorNotification) {
  auto& ov = make_overlay();
  open_result_ = false;
  ov.open_target("/no/such/dir", "the sandbox folder");
  ASSERT_EQ(opened_, (std::vector<std::string>{"/no/such/dir"}));
  auto items = notifications_->active();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].content.kind, wish::notification_kind::error);
  EXPECT_NE(items[0].content.title.find("sandbox folder"), std::string::npos);

  open_result_ = true;
  ov.open_target("https://example.com", "a link");
  EXPECT_EQ(notifications_->active().size(), 1u) << "success posts nothing";
}

TEST_F(ServerOverlayTest, ToastsDrawAboveTheOverlayAndDismissOnClick) {
  frame();
  EXPECT_FALSE(toasts_visible_);
  auto& ov = make_overlay();
  ov.set_open(true);
  notifications_->post(note("hello", 0ms));
  frame();
  frame();
  EXPECT_TRUE(toasts_visible_);
  const int toasts = display_index("##wish_notifications");
  ASSERT_NE(toasts, -1);
  EXPECT_GT(toasts, display_index("##wish_server_overlay"));

  // Click the toast (right part of its window: it slides in from the right).
  ImGuiWindow* w = ImGui::FindWindowByName("##wish_notifications");
  ASSERT_NE(w, nullptr);
  for (int i = 0; i < 30; ++i) // let the slide-in finish
    frame();
  const ImVec2 at{w->Pos.x + w->Size.x - 40.0f, w->Pos.y + w->Size.y * 0.5f};
  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(at.x, at.y);
  frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  frame();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  frame();
  EXPECT_TRUE(notifications_->active().empty());
}
