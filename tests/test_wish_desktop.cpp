// MIT License © 2025 Binary Dice Games
// Tests for app/wish_cli/desktop/wish_desktop_app.hpp -- the desktop shell
// (DesktopShell/DesktopApp chrome, app launching, pins, theme, running apps).
#include "app/wish_cli/desktop/wish_desktop_app.hpp"

#include <auth/local_auth_module.hpp>
#include <client/app_registry.hpp>
#include <context/persistent_store.hpp>
#include <server/server.hpp>

#include "src/rmi/rmi.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

// wish_desktop_app.cpp DECLAREs these (DEFINEd in main.cpp / standalone_main.cpp
// for the real binaries, neither of which this test links) and DEFINEs the
// upstream_* flags itself -- mirrors extern/bison/tests/bridge_app_tests.cpp.
DEFINE_string(cmd, "", "test default");
DEFINE_string(verbose, "none", "test default");
DEFINE_bool(debugger, false, "test default");
DEFINE_int32(timeout, 30000, "test default");

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

namespace {

// Two fake apps -- this test links no modules, so these are the whole
// registry. Listed by the shell sorted by collection then name: calc
// ("app0"), nano ("app1"); nano is one of the default pins.
const bool kAppsRegistered = [] {
  wish::register_app(
      {.name = "nano",
       .organization = "test",
       .collection = "apps",
       .description = "Fake editor",
       .icon = "res/icons/edit.png",
       .params = {},
       .run = [](wish::wish_app_host&) {}});
  wish::register_app(
      {.name = "calc",
       .organization = "test",
       .collection = "apps",
       .description = "Fake calculator",
       .icon = "res/icons/calculate.png",
       .params = {},
       .run = [](wish::wish_app_host&) {}});
  return true;
}();

// Records launches instead of starting processes.
class fake_launcher : public wish::process_launcher_iface {
 public:
  std::vector<wish::launch_request> requests;

  std::optional<int> launch(const wish::launch_request& request, std::string&) override {
    requests.push_back(request);
    return 1234;
  }
  void terminate_all() override {}
};

// Captures the wish::context of the single session wish_desktop opens on the
// upstream server, so tests can inspect the chrome elements it created.
class tracking_upstream_server : public wish::server {
 public:
  tracking_upstream_server(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}

  wish::context* last_session{nullptr};

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
  }
};

std::filesystem::path test_store_dir() {
  return std::filesystem::temp_directory_path() /
      ("wish_desktop_test_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_" +
       std::to_string(bdg::bison::rmi::shared::generate_id().id));
}

dynamic upstream_params() {
  dynamic p;
  p["username"_key] = std::string{"tester"};
  return p;
}

// Fixture: a real `wish::server` (upstream, with a user store) reachable via
// in-process memory transports, and a `wish_desktop` bridging a downstream
// memory transport to it.
class WishDesktopTest : public ::testing::Test {
 protected:
  std::filesystem::path store_dir_ = test_store_dir();
  memory_server_transport upstream_transport_;
  tracking_upstream_server upstream_srv_{upstream_transport_, std::make_unique<wish::null_renderer>()};

  memory_server_transport downstream_transport_;
  wish::wish_desktop desktop_{
      downstream_transport_,
      std::make_unique<memory_client_transport>(upstream_transport_.connect()),
      upstream_params()};
  fake_launcher launcher_;

  void SetUp() override {
    ASSERT_TRUE(kAppsRegistered);
    upstream_srv_.set_store_dir(store_dir_);
    upstream_srv_.start(std::make_shared<wish::local_auth_module>());
    desktop_.set_launcher(&launcher_);
    desktop_.set_client_command({"wish", "client"});
    desktop_.set_launch_env({{"WISH_HOST", "127.0.0.1"}});
    desktop_.start();
  }

  void TearDown() override {
    desktop_.stop();
    upstream_srv_.stop();
    std::error_code ec;
    std::filesystem::remove_all(store_dir_, ec);
  }

  wish::context& session() {
    EXPECT_NE(upstream_srv_.last_session, nullptr);
    return *upstream_srv_.last_session;
  }

  /// The upstream element at dot-path @p path (as created by the shell
  /// template), or nullptr.
  wish::ui_element_ptr element(const std::string& path) {
    wish::ui_element_ptr out;
    session().ui_objects.with(path, [&](const wish::ui_element_ptr& e) { out = e; });
    return out;
  }

  bdg::bison::key_t shell_id() {
    auto shell = element("");
    return shell ? shell->get_as<bdg::bison::key_t>("__wish_id"_key, bdg::bison::key_t{}) : bdg::bison::key_t{};
  }

  /// Delivers a shell event the way the render loop does after a frame.
  void emit(const char* event, dynamic payload = {}) {
    wish::context& sess = session();
    ASSERT_TRUE(sess.emit_event);
    sess.emit_event(shell_id(), bdg::bison::key_t{event}, std::move(payload));
  }

  /// Waits until @p pred holds (after letting the desktop finish its queued
  /// upstream work), up to a bounded timeout.
  template <typename Pred>
  bool eventually(Pred&& pred) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
      desktop_.flush();
      if (pred())
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
  }
};

} // namespace

// ── Shell construction ───────────────────────────────────────────────────────

TEST_F(WishDesktopTest, BuildChromeCreatesDesktopShellWithOneAppPerRegisteredApp) {
  desktop_.build_chrome();

  wish::context& sess = session();
  ASSERT_EQ(sess.top_level_objects.size(), size_t{1});
  EXPECT_EQ(sess.top_level_objects.begin()->second->class_key(), bdg::bison::key_t{"DesktopShell"});
  EXPECT_EQ(element("")->get_as<std::string>("user"_key, std::string{}), std::string{});

  auto calc = element("app0");
  auto nano = element("app1");
  ASSERT_TRUE(calc);
  ASSERT_TRUE(nano);
  EXPECT_EQ(calc->class_key(), bdg::bison::key_t{"DesktopApp"});
  EXPECT_EQ(calc->get_as<std::string>("app"_key, std::string{}), "test/apps/calc");
  EXPECT_EQ(calc->get_as<std::string>("label"_key, std::string{}), "calc");
  EXPECT_EQ(calc->get_as<std::string>("icon"_key, std::string{}), "res/icons/calculate.png");
  EXPECT_EQ(nano->get_as<std::string>("app"_key, std::string{}), "test/apps/nano");
  EXPECT_EQ(nano->get_as<std::string>("description"_key, std::string{}), "Fake editor");
}

TEST_F(WishDesktopTest, BuildChromeIsIdempotent) {
  desktop_.build_chrome();
  desktop_.build_chrome();
  desktop_.build_chrome();

  // A second top-level object would appear if it ran twice.
  EXPECT_EQ(session().top_level_objects.size(), size_t{1});
}

TEST_F(WishDesktopTest, DefaultPinsApplyWithoutSavedSettings) {
  desktop_.build_chrome();

  // "nano" is a default pin and desktop icon; "calc" is neither.
  EXPECT_TRUE(element("app1")->get_as<bool>("pinned"_key, false));
  EXPECT_TRUE(element("app1")->get_as<bool>("on_desktop"_key, false));
  EXPECT_FALSE(element("app0")->get_as<bool>("pinned"_key, true));
  EXPECT_FALSE(element("app0")->get_as<bool>("on_desktop"_key, true));
  EXPECT_EQ(desktop_.settings().pinned, std::vector<std::string>{"test/apps/nano"});
}

// ── Shell events ─────────────────────────────────────────────────────────────

TEST_F(WishDesktopTest, QuitEventInvokesRequestQuit) {
  desktop_.build_chrome();
  emit("quit");
  // wait_for_quit() blocks until request_quit() fires; a hang here means the
  // "quit" handler didn't call it.
  desktop_.wait_for_quit();
}

TEST_F(WishDesktopTest, LaunchEventRunsTheClientCommandForTheApp) {
  desktop_.build_chrome();
  dynamic p;
  p["app"_key] = std::string{"test/apps/calc"};
  emit("launch", std::move(p));

  ASSERT_TRUE(eventually([&] { return !launcher_.requests.empty(); }));
  const auto& req = launcher_.requests.front();
  EXPECT_EQ(req.argv, (std::vector<std::string>{"wish", "client", "--run=test/apps/calc"}));
  EXPECT_EQ(req.env, (std::vector<std::pair<std::string, std::string>>{{"WISH_HOST", "127.0.0.1"}}));
}

TEST_F(WishDesktopTest, LaunchWithoutLauncherFails) {
  desktop_.set_launcher(nullptr);
  EXPECT_FALSE(desktop_.launch_app("test/apps/calc"));
}

TEST_F(WishDesktopTest, PinEventUpdatesTheAppAndPersists) {
  desktop_.build_chrome();
  dynamic p;
  p["app"_key] = std::string{"test/apps/calc"};
  p["pinned"_key] = true;
  emit("pin", std::move(p));

  ASSERT_TRUE(eventually([&] { return element("app0")->get_as<bool>("pinned"_key, false); }));
  auto pinned = desktop_.settings().pinned;
  EXPECT_NE(std::find(pinned.begin(), pinned.end(), "test/apps/calc"), pinned.end());

  ASSERT_TRUE(session().user_store);
  auto saved = session().user_store->get("wish.desktop");
  ASSERT_TRUE(saved.has_value());
  const field* list = saved->findField("pinned"_key);
  ASSERT_TRUE(list && list->is<dynamic_ptr>());
  bool found = false;
  list->as<dynamic_ptr>()->forEach([&](bdg::bison::key_t, const field& v) {
    if (v.is<std::string>() && v.as<std::string>() == "test/apps/calc")
      found = true;
  });
  EXPECT_TRUE(found);
}

TEST_F(WishDesktopTest, DesktopIconEventUpdatesTheApp) {
  desktop_.build_chrome();
  dynamic p;
  p["app"_key] = std::string{"test/apps/nano"};
  p["on_desktop"_key] = false;
  emit("desktop_icon", std::move(p));

  ASSERT_TRUE(eventually([&] { return !element("app1")->get_as<bool>("on_desktop"_key, true); }));
  EXPECT_TRUE(desktop_.settings().on_desktop.empty());
}

TEST_F(WishDesktopTest, ThemeEventAppliesAndPersistsThePreset) {
  desktop_.build_chrome();
  dynamic p;
  p["preset"_key] = std::string{"dark"};
  emit("theme", std::move(p));

  ASSERT_TRUE(eventually([&] { return desktop_.settings().theme == "dark"; }));
  ASSERT_TRUE(eventually([&] {
    auto saved = session().user_store->get("wish.desktop");
    return saved && saved->get_as<std::string>("theme"_key, std::string{}) == "dark";
  }));
}

TEST_F(WishDesktopTest, SavedSettingsAreRestored) {
  dynamic entry;
  auto pinned = std::make_shared<dynamic>();
  (*pinned)[bdg::bison::key_t{0u}] = std::string{"test/apps/calc"};
  entry["pinned"_key] = dynamic_ptr{pinned};
  entry["desktop"_key] = dynamic_ptr{std::make_shared<dynamic>()};
  entry["theme"_key] = std::string{"light"};
  // The session exists once the bridge connected upstream (start()).
  session().user_store->set("wish.desktop", entry);

  desktop_.build_chrome();

  EXPECT_TRUE(element("app0")->get_as<bool>("pinned"_key, false));
  EXPECT_FALSE(element("app1")->get_as<bool>("pinned"_key, true));
  EXPECT_FALSE(element("app1")->get_as<bool>("on_desktop"_key, true));
  EXPECT_EQ(desktop_.settings().theme, "light");
}

// ── Running apps ─────────────────────────────────────────────────────────────

TEST_F(WishDesktopTest, ConnectedClientIsPublishedOnItsApp) {
  desktop_.build_chrome();

  client app_client(std::make_unique<memory_client_transport>(downstream_transport_.connect()));
  dynamic params;
  params["app"_key] = std::string{"test/apps/nano"};
  app_client.connect(std::move(params));

  auto sessions_of_nano = [&] {
    const field* f = element("app1")->findField("sessions"_key);
    return f && f->is<std::vector<int32_t>>() ? f->as<std::vector<int32_t>>() : std::vector<int32_t>{};
  };
  ASSERT_TRUE(eventually([&] { return sessions_of_nano().size() == 1; }));
  EXPECT_EQ(element("")->get_as<int32_t>("clients"_key, 0), 1);

  app_client.disconnect();
  ASSERT_TRUE(eventually([&] { return sessions_of_nano().empty(); }));
  EXPECT_EQ(element("")->get_as<int32_t>("clients"_key, -1), 0);
}

TEST_F(WishDesktopTest, WaitForQuitForTimesOutThenReturnsTrueAfterRequestQuit) {
  // Before request_quit() is called, wait_for_quit_for() must time out and
  // report false -- this is what lets wish_desktop_app::wait_for_shutdown()
  // poll an active_term_ for exit alongside the Quit menu item instead of
  // blocking on wait_for_quit() forever.
  EXPECT_FALSE(desktop_.wait_for_quit_for(std::chrono::milliseconds{20}));

  desktop_.request_quit();

  EXPECT_TRUE(desktop_.wait_for_quit_for(std::chrono::milliseconds{20}));
}

// ── process_launcher ─────────────────────────────────────────────────────────

TEST(ProcessLauncherTest, SelfExePathExists) {
  auto self = wish::process_launcher::self_exe_path();
  ASSERT_FALSE(self.empty());
  EXPECT_TRUE(std::filesystem::exists(self));
}

TEST(ProcessLauncherTest, LaunchRunsTheProgramAndCapturesItsOutput) {
  auto dir = test_store_dir();
  std::filesystem::create_directories(dir);
  auto log = dir / "out.log";

  wish::process_launcher launcher;
  wish::launch_request req;
  req.argv = {CMAKE_COMMAND_PATH, "-E", "echo", "hello-from-child"};
  req.log_path = log;
  std::string error;
  auto pid = launcher.launch(req, error);
  ASSERT_TRUE(pid.has_value()) << error;

  // The child exits on its own; the launcher reaps it.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (!launcher.running().empty() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  EXPECT_TRUE(launcher.running().empty());

  std::ifstream in(log);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("hello-from-child"), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(ProcessLauncherTest, EnvironmentOverridesReachTheChild) {
  auto dir = test_store_dir();
  std::filesystem::create_directories(dir);
  auto log = dir / "env.log";

  wish::process_launcher launcher;
  wish::launch_request req;
  req.argv = {CMAKE_COMMAND_PATH, "-E", "environment"};
  req.env = {{"WISH_DESKTOP_TEST_VAR", "xyz"}};
  req.log_path = log;
  std::string error;
  ASSERT_TRUE(launcher.launch(req, error).has_value()) << error;

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (!launcher.running().empty() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  std::ifstream in(log);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("WISH_DESKTOP_TEST_VAR=xyz"), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(ProcessLauncherTest, TerminateAllStopsRunningChildren) {
  wish::process_launcher launcher;
  wish::launch_request req;
  req.argv = {CMAKE_COMMAND_PATH, "-E", "sleep", "30"};
  std::string error;
  ASSERT_TRUE(launcher.launch(req, error).has_value()) << error;
  EXPECT_EQ(launcher.running().size(), size_t{1});

  launcher.terminate_all();
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (!launcher.running().empty() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  EXPECT_TRUE(launcher.running().empty());
}

TEST(ProcessLauncherTest, LaunchOfMissingProgramReportsAnError) {
  wish::process_launcher launcher;
  wish::launch_request req;
  req.argv = {"/nonexistent/wish-desktop-test-program"};
  std::string error;
  EXPECT_FALSE(launcher.launch(req, error).has_value());
  EXPECT_FALSE(error.empty());
}
