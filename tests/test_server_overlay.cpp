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
#include <auth/local_auth_module.hpp>
#include <client/client.hpp>
#include <server/notification_service.hpp>
#include <server/notifications.hpp>
#include <server/perf_stats.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_common.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/shared/ids.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
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

// ── notification_service (client notifications) ─────────────────────────────

TEST(NotificationService, MarksSourceAndClipsText) {
  auto center = std::make_shared<wish::notification_center>();
  auto svc = wish::notification_service::instantiate(center, "alice");
  wish::notification n = note(std::string(500, 'T'), 10min);
  n.body = std::string(5000, 'b');
  n.source = "the server"; // a client can't choose its own source
  svc->post(std::move(n));
  auto items = center->active();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].content.source, "alice");
  EXPECT_EQ(items[0].content.title.size(), wish::notification_service::kMaxTitle);
  EXPECT_EQ(items[0].content.body.size(), wish::notification_service::kMaxBody);
  EXPECT_EQ(items[0].expires - items[0].posted,
            std::chrono::milliseconds{wish::notification_service::kMaxDurationMs});
  EXPECT_THROW(svc->post(note("")), std::invalid_argument);
}

TEST(NotificationService, AnonymousSourceAndLateIdentity) {
  auto center = std::make_shared<wish::notification_center>();
  auto svc = wish::notification_service::instantiate(center, "");
  svc->post(note("a"));
  svc->set_source("bob");
  svc->post(note("b"));
  auto items = center->active();
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0].content.source, "an anonymous app");
  EXPECT_EQ(items[1].content.source, "bob");
}

TEST(NotificationService, KeepsAtMostFourPerSession) {
  auto center = std::make_shared<wish::notification_center>();
  center->post(note("server's own"));
  auto svc = wish::notification_service::instantiate(center, "alice");
  for (int i = 0; i < 7; ++i)
    svc->post(note("n" + std::to_string(i)));
  auto items = center->active();
  ASSERT_EQ(items.size(), 1u + wish::notification_service::kMaxPerSession);
  EXPECT_EQ(items[0].content.title, "server's own") << "a client never pushes out others' notifications";
  EXPECT_EQ(items[1].content.title, "n3");
}

TEST(NotificationService, TagsAndDismissAreScopedToTheSession) {
  auto center = std::make_shared<wish::notification_center>();
  auto a = wish::notification_service::instantiate(center, "alice");
  auto b = wish::notification_service::instantiate(center, "bob");
  const uint64_t a1 = a->post(note("alice v1", 6s, "progress"));
  const uint64_t b1 = b->post(note("bob v1", 6s, "progress"));
  EXPECT_NE(a1, b1) << "the same tag in two sessions doesn't collide";
  EXPECT_EQ(a->post(note("alice v2", 6s, "progress")), a1);
  EXPECT_FALSE(a->dismiss(b1)) << "can't dismiss another session's notification";
  EXPECT_EQ(center->active().size(), 2u);
  EXPECT_TRUE(a->dismiss(a1));
  EXPECT_EQ(center->active().size(), 1u);
}

TEST(NotificationService, SessionEndRemovesItsNotifications) {
  auto center = std::make_shared<wish::notification_center>();
  {
    auto svc = wish::notification_service::instantiate(center, "alice");
    svc->post(note("sticky", 0ms));
  }
  EXPECT_TRUE(center->active().empty());
}

namespace {

// Runs @p body inside on_session(), connected as @p username.
class notify_test_client : public wish::client {
 public:
  using wish::client::client;
  std::function<void(notify_test_client&)> body;

 protected:
  void on_session() override {
    if (body)
      body(*this);
  }
};

} // namespace

TEST(NotificationService, ClientsPostOverRmi) {
  rmi::transport::memory_server_transport transport;
  auto renderer = std::make_unique<wish::null_renderer>();
  auto center = renderer->notifications();
  wish::server srv{transport, std::move(renderer)};
  srv.start(std::make_shared<wish::local_auth_module>());

  auto run_as = [&](const std::string& username, std::function<void(notify_test_client&)> body) {
    notify_test_client c{transport.connect()};
    c.body = std::move(body);
    dynamic params;
    if (!username.empty())
      params["username"_key] = username;
    c.run(params);
  };

  bool has = false;
  uint64_t id = 0;
  bool dismissed_unknown = true;
  std::vector<wish::active_notification> seen;
  run_as("alice", [&](notify_test_client& c) {
    has = c.has_notifications();
    wish::notification n = note("Backup finished", 0ms);
    n.body = "42 files";
    n.kind = wish::notification_kind::success;
    n.keys = "Ctrl+B";
    id = c.notify(n).get();
    dismissed_unknown = c.dismiss_notification(id + 1000).get();
    seen = center->active();
    EXPECT_THROW(c.notify(note("")).get(), std::exception);
  });
  EXPECT_TRUE(has);
  EXPECT_GT(id, 0u);
  EXPECT_FALSE(dismissed_unknown);
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].id, id);
  EXPECT_EQ(seen[0].content.title, "Backup finished");
  EXPECT_EQ(seen[0].content.body, "42 files");
  EXPECT_EQ(seen[0].content.kind, wish::notification_kind::success);
  EXPECT_EQ(seen[0].content.keys, "Ctrl+B");
  EXPECT_EQ(seen[0].content.source, "alice");
  // The session is torn down on the server's worker thread: wait for it.
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!center->active().empty() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  EXPECT_TRUE(center->active().empty()) << "the sticky one went with its session";

  std::string anon_source;
  run_as("", [&](notify_test_client& c) {
    c.notify(note("hi")).get();
    anon_source = center->active().at(0).content.source;
  });
  EXPECT_EQ(anon_source, "an anonymous app");
  srv.stop();
}

// ── perf_recorder ───────────────────────────────────────────────────────────

TEST(PerfRecorder, ClosesOneSamplePerSecond) {
  wish::perf_recorder perf;
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(perf.due(t0));
  perf.close_sample(t0, 0, 1.0, 0); // baseline only
  EXPECT_TRUE(perf.history().empty());
  EXPECT_FALSE(perf.due(t0 + 500ms));

  perf.record_frame(2ms);
  perf.record_frame(6ms);
  ASSERT_TRUE(perf.due(t0 + 1s));
  perf.close_sample(t0 + 1s, 3, 1.5, uint64_t{64} << 20);
  ASSERT_EQ(perf.history().size(), 1u);
  const auto s = *perf.latest();
  EXPECT_FLOAT_EQ(s.frame_ms_avg, 4.0f);
  EXPECT_FLOAT_EQ(s.frame_ms_max, 6.0f);
  EXPECT_FLOAT_EQ(s.frames, 2.0f);
  EXPECT_NEAR(s.cpu_percent, 50.0f, 0.01f);
  EXPECT_FLOAT_EQ(s.memory_mb, 64.0f);
  EXPECT_FLOAT_EQ(s.clients, 3.0f);

  perf.close_sample(t0 + 3s, 3, 1.5, 0); // idle and late: no frames, 0 % CPU
  EXPECT_FLOAT_EQ(perf.latest()->frames, 0.0f);
  EXPECT_FLOAT_EQ(perf.latest()->frame_ms_avg, 0.0f);
  EXPECT_FLOAT_EQ(perf.latest()->cpu_percent, 0.0f);
}

TEST(PerfRecorder, KeepsTwoMinutes) {
  wish::perf_recorder perf;
  auto t = std::chrono::steady_clock::now();
  for (size_t i = 0; i < wish::perf_recorder::kHistory + 10; ++i, t += 1s)
    perf.close_sample(t, i, -1.0, 0);
  EXPECT_EQ(perf.history().size(), wish::perf_recorder::kHistory);
  EXPECT_FLOAT_EQ(perf.history().back().clients, static_cast<float>(wish::perf_recorder::kHistory + 9));
  EXPECT_FLOAT_EQ(perf.history().back().cpu_percent, 0.0f) << "unknown CPU time reads 0";
}

TEST(MachineInfo, ProcessFigures) {
  EXPECT_GE(wish::process_cpu_seconds(), 0.0);
  EXPECT_GT(wish::process_memory_bytes(), 0u);
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

TEST(ServerOverlayDetail, ScreenshotPathNeverOverwrites) {
  temp_dir dir;
  const std::time_t t = 1700000000;
  const auto first = wish::detail::screenshot_path(dir.path, t);
  EXPECT_EQ(first.parent_path(), dir.path);
  EXPECT_EQ(first.extension(), ".png");
  EXPECT_EQ(first.filename().string().rfind("wish-", 0), 0u);
  std::ofstream(first) << "x";
  const auto second = wish::detail::screenshot_path(dir.path, t);
  EXPECT_NE(first, second);
  EXPECT_EQ(second.stem().string(), first.stem().string() + "-2");
}

TEST(ServerOverlayDetail, NewestTraceIgnoresOtherFiles) {
  temp_dir dir;
  EXPECT_TRUE(wish::detail::newest_trace(dir.path).empty());
  std::ofstream(dir.path / "old.perfetto-trace") << "";
  std::ofstream(dir.path / "notes.txt") << "";
  std::filesystem::last_write_time(dir.path / "old.perfetto-trace",
                                   std::filesystem::file_time_type::clock::now() - 1h);
  std::ofstream(dir.path / "new.perfetto-trace") << "";
  EXPECT_EQ(wish::detail::newest_trace(dir.path).filename(), "new.perfetto-trace");
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
    info.request_screenshot = [this](std::function<void(std::vector<uint8_t>)> done) {
      if (!screen_available_)
        return false;
      screenshot_cb_ = std::move(done);
      ++screenshot_requests_;
      return true;
    };
    info.screenshot_dir = dir_.path / "shots";
    info.perf = &perf_;
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
    // Like host_renderer: no toasts in a screenshot frame.
    toasts_visible_ = !(overlay_ && overlay_->capturing()) && wish::render_notifications(*notifications_);
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
  wish::perf_recorder perf_;
  std::function<void(std::vector<uint8_t>)> screenshot_cb_;
  int screenshot_requests_{0};
  bool screen_available_{true};
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
  for (int i = 0; i < 5; ++i)
    perf_.close_sample(std::chrono::steady_clock::now() + std::chrono::seconds{i}, 1, 0.1 * i, 1 << 20);
  for (auto p : {wish::server_overlay::page::overview, wish::server_overlay::page::clients,
                 wish::server_overlay::page::performance, wish::server_overlay::page::settings,
                 wish::server_overlay::page::logs,
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

// ── Screenshots ─────────────────────────────────────────────────────────────

TEST_F(ServerOverlayTest, ScreenshotHidesTheOverlaySavesThePngAndComesBack) {
  auto& ov = make_overlay();
  ov.set_open(true);
  notifications_->post(note("visible toast", 0ms));
  frame();
  frame();
  ASSERT_TRUE(ImGui::FindWindowByName("##wish_server_overlay")->Active);

  ov.take_screenshot();
  EXPECT_TRUE(ov.capturing());
  frame(); // the capture frame: requested, nothing of ours drawn
  EXPECT_EQ(screenshot_requests_, 1);
  EXPECT_FALSE(ImGui::FindWindowByName("##wish_server_overlay")->Active);
  EXPECT_FALSE(toasts_visible_);
  ov.take_screenshot(); // ignored while one runs
  frame();
  EXPECT_EQ(screenshot_requests_, 1);

  const std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  screenshot_cb_(png); // as the renderer would, from any thread
  frame();
  EXPECT_FALSE(ov.capturing());
  EXPECT_TRUE(ov.is_open());
  frame();
  EXPECT_TRUE(ImGui::FindWindowByName("##wish_server_overlay")->Active) << "the overlay comes back";

  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir_.path / "shots"))
    files.push_back(e.path());
  ASSERT_EQ(files.size(), 1u);
  std::ifstream in(files[0], std::ios::binary);
  std::vector<uint8_t> saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(saved, png);
  auto items = notifications_->active();
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[1].content.title, "Screenshot saved");
  EXPECT_NE(items[1].content.body.find(files[0].filename().string()), std::string::npos);
}

TEST_F(ServerOverlayTest, ScreenshotWithNothingToCaptureReportsAnError) {
  auto& ov = make_overlay();
  screen_available_ = false; // e.g. no browser shows the web renderer
  ov.set_open(true);
  ov.take_screenshot();
  frame();
  EXPECT_FALSE(ov.capturing());
  auto items = notifications_->active();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].content.kind, wish::notification_kind::error);
}

TEST_F(ServerOverlayTest, EmptyCaptureReportsAnError) {
  auto& ov = make_overlay();
  ov.take_screenshot(); // works with the overlay closed too
  frame();
  ASSERT_TRUE(screenshot_cb_);
  screenshot_cb_({});
  frame();
  EXPECT_FALSE(ov.capturing());
  EXPECT_FALSE(ov.is_open()) << "it wasn't open before";
  auto items = notifications_->active();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].content.kind, wish::notification_kind::error);
}

TEST_F(ServerOverlayTest, ToastsStayAboveTheOverlayActionBar) {
  auto& ov = make_overlay();
  EXPECT_EQ(ov.bottom_bar_height(), 0.0f);
  ov.set_open(true);
  frame();
  const float bar = ov.bottom_bar_height();
  ASSERT_GT(bar, 0.0f);
  notifications_->post(note("hello", 0ms));
  renderer_->begin_frame();
  ov.render({session_});
  wish::render_notifications(*notifications_, wish::notification_corner::bottom_right, bar);
  renderer_->end_frame();
  ImGuiWindow* w = ImGui::FindWindowByName("##wish_notifications");
  ASSERT_NE(w, nullptr);
  EXPECT_LE(w->Pos.y + w->Size.y, ImGui::GetIO().DisplaySize.y - bar);
}

// ── Themes ──────────────────────────────────────────────────────────────────

TEST(StyleThemes, ClientListsTheServersThemes) {
  rmi::transport::memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();
  std::vector<wish::theme_info> themes;
  {
    notify_test_client c{transport.connect()};
    c.body = [&](notify_test_client& self) { themes = self.list_themes().get(); };
    c.run(dynamic{});
  }
  srv.stop();
  const auto expected = wish::registered_themes();
  ASSERT_EQ(themes.size(), expected.size());
  for (size_t i = 0; i < themes.size(); ++i) {
    EXPECT_EQ(themes[i].name, expected[i].name);
    EXPECT_EQ(themes[i].label, expected[i].label);
    EXPECT_EQ(themes[i].is_light, expected[i].is_light);
  }
  EXPECT_EQ(themes.front().name, "wish");
}

TEST(ServerOverlayDetail, ThemeOptionsAreTheOverlayLookThenEveryTheme) {
  const auto options = wish::detail::overlay_theme_options();
  ASSERT_EQ(options.size(), 1 + wish::registered_themes().size());
  EXPECT_EQ(options[0].value, wish::kOverlayTheme);
  for (const auto& t : wish::registered_themes()) {
    EXPECT_TRUE(std::any_of(options.begin(), options.end(), [&](const auto& o) { return o.value == t.name; }))
        << t.name;
  }
  EXPECT_TRUE(wish::detail::overlay_palette_is_builtin(wish::kOverlayTheme));
  EXPECT_TRUE(wish::detail::overlay_palette_is_builtin("no-such-theme"));
  EXPECT_FALSE(wish::detail::overlay_palette_is_builtin("dark"));
}

TEST_F(ServerOverlayTest, ThemeIsSavedInTheServerStoreAndFollowed) {
  auto& ov = make_overlay();
  EXPECT_EQ(ov.theme(), wish::kOverlayTheme);
  const auto setting = wish::detail::overlay_theme_setting();
  ov.set_theme("dark");
  EXPECT_EQ(ov.theme(), "dark");
  EXPECT_EQ(wish::settings::text_of(stores_->server_store().get(), setting), "dark");

  // A new overlay (the next run) starts with it.
  overlay_.reset();
  auto& again = make_overlay();
  EXPECT_EQ(again.theme(), "dark");

  // Changed elsewhere (the Settings page): followed within a second.
  ASSERT_EQ(wish::settings::write_setting(*stores_->server_store(), setting, std::string{"classic"}), "");
  again.set_open(true);
  frame();
  ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
  std::this_thread::sleep_for(1100ms);
  frame();
  EXPECT_EQ(again.theme(), "classic");

  again.set_theme("no-such-theme");
  EXPECT_EQ(again.theme(), wish::kOverlayTheme);
}

TEST_F(ServerOverlayTest, EveryThemeRenders) {
  auto& ov = make_overlay();
  ov.set_open(true);
  for (const auto& opt : wish::detail::overlay_theme_options()) {
    ov.set_theme(opt.value);
    for (auto p : {wish::server_overlay::page::overview, wish::server_overlay::page::settings}) {
      ov.set_page(p);
      frame();
    }
    EXPECT_EQ(ov.theme(), opt.value);
  }
}

// ── Shutdown confirmation ───────────────────────────────────────────────────

namespace {

// Whether the "Shut down server?" modal was drawn in the last frame.
bool shutdown_popup_shown() {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  for (ImGuiWindow* w : g.Windows) {
    if (w->WasActive && std::strstr(w->Name, "Shut down server?"))
      return true;
  }
  return false;
}

} // namespace

TEST_F(ServerOverlayTest, ShutdownRequestShowsTheConfirmationWithoutTheOverlay) {
  auto& ov = make_overlay();
  frame();
  ov.request_shutdown();
  EXPECT_TRUE(ov.confirming_shutdown());
  frame();
  frame();
  EXPECT_FALSE(ov.is_open());
  EXPECT_TRUE(shutdown_popup_shown());
  EXPECT_EQ(shutdowns_, 0); // nothing stops before the operator confirms

  press(ImGuiKey_Escape); // Cancel
  frame();
  EXPECT_FALSE(ov.confirming_shutdown());
  EXPECT_FALSE(shutdown_popup_shown());
  EXPECT_EQ(shutdowns_, 0);

  ov.request_shutdown(); // asking again shows it again
  frame();
  frame();
  EXPECT_TRUE(shutdown_popup_shown());
}

TEST_F(ServerOverlayTest, ShutdownConfirmationSurvivesTogglingTheOverlay) {
  auto& ov = make_overlay();
  frame();
  ov.request_shutdown();
  frame();
  frame();
  ov.set_open(true);
  frame();
  frame();
  EXPECT_TRUE(shutdown_popup_shown());
  ov.set_open(false);
  frame();
  frame();
  EXPECT_TRUE(shutdown_popup_shown());
  EXPECT_TRUE(ov.confirming_shutdown());
}
