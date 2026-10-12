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
#include <functional>
#include <iterator>
#include <sstream>
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
  std::filesystem::path config_file_ = store_dir_ / "desktop" / "desktop.yaml";
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
    desktop_.set_config_file(config_file_);
    desktop_.set_config_poll_interval(std::chrono::milliseconds{20});
    desktop_.start();
  }

  void TearDown() override {
    desktop_.stop_tasks();
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

  /// The DesktopShell (by class: once the desktop adds an entry for an
  /// unknown app, element("") is that template's root instead).
  wish::ui_element_ptr shell() {
    for (const auto& [key, root] : session().top_level_objects)
      if (root && root->class_key() == bdg::bison::key_t{"DesktopShell"})
        return root;
    return nullptr;
  }

  bdg::bison::key_t shell_id() {
    auto sh = shell();
    return sh ? sh->get_as<bdg::bison::key_t>("__wish_id"_key, bdg::bison::key_t{}) : bdg::bison::key_t{};
  }

  /// Writes the desktop configuration file.
  void write_config(const std::string& text) {
    std::filesystem::create_directories(config_file_.parent_path());
    std::ofstream(config_file_, std::ios::binary | std::ios::trunc) << text;
  }

  /// The node of the current shell's tree with id @p id (a DesktopApp entry
  /// or DesktopMenu folder), or nullptr.
  wish::ui_element_ptr entry(const std::string& id) {
    auto sh = shell();
    if (!sh)
      return nullptr;
    wish::ui_element_ptr found;
    std::function<void(const wish::ui_element&)> walk = [&](const wish::ui_element& parent) {
      const field* children = parent.findField("children"_key);
      if (!children || !children->is<dynamic_ptr>() || !children->as<dynamic_ptr>())
        return;
      children->as<dynamic_ptr>()->forEach([&](bdg::bison::key_t, const field& f) {
        if (found || !f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
          return;
        auto child = std::dynamic_pointer_cast<wish::ui_element>(std::shared_ptr<dynamic>(f.as<dynamic_ptr>()));
        if (!child)
          return;
        if (child->get_as<std::string>("id"_key, std::string{}) == id)
          found = child;
        else
          walk(*child);
      });
    };
    walk(*sh);
    return found;
  }

  size_t shell_count() {
    size_t n = 0;
    for (const auto& [key, root] : session().top_level_objects)
      if (root && root->class_key() == bdg::bison::key_t{"DesktopShell"})
        ++n;
    return n;
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

// A configuration exercising every kind of entry.
const char* kCustomConfig = R"(
start_menu:
  pinned: [ed]
  desktop: [ed, test/apps/calc]
  items:
    - label: Tools
      id: tools
      icon: res/icons/code.png
      tooltip: Some tools
      items:
        - label: Editor
          id: ed
          icon: res/icons/edit.png
          tooltip: Edits things
          run: [editor, --flag, "{config_file}"]
          app: ext/editor
          env: {EDITOR_MODE: dark}
          cwd: /tmp
    - apps: all
    - separator: true
    - label: Dark
      id: dark
      action: set_theme
      args: {preset: dark}
  power:
    - label: Quit desktop
      id: bye
      action: quit
)";

// ── Shell construction ───────────────────────────────────────────────────────

TEST_F(WishDesktopTest, MissingConfigGetsTheDefaultWrittenAndUsed) {
  desktop_.build_chrome();

  std::ifstream in(config_file_);
  std::stringstream ss;
  ss << in.rdbuf();
  EXPECT_EQ(ss.str(), wish::desktop::default_config_text());

  EXPECT_EQ(shell_count(), size_t{1});
  // `apps: all`: a folder per collection with the registered apps.
  auto folder = entry("apps");
  ASSERT_TRUE(folder);
  EXPECT_EQ(folder->class_key(), bdg::bison::key_t{"DesktopMenu"});
  auto calc = entry("test/apps/calc");
  ASSERT_TRUE(calc);
  EXPECT_EQ(calc->class_key(), bdg::bison::key_t{"DesktopApp"});
  EXPECT_EQ(calc->get_as<std::string>("app"_key, std::string{}), "test/apps/calc");
  EXPECT_EQ(calc->get_as<std::string>("icon"_key, std::string{}), "res/icons/calculate.png");
  EXPECT_EQ(calc->get_as<std::string>("tooltip"_key, std::string{}), "Fake calculator");
  // The default power menu.
  auto power = entry("power");
  ASSERT_TRUE(power);
  EXPECT_EQ(power->get_as<std::string>("role"_key, std::string{}), "power");
  EXPECT_TRUE(entry("power/quit-desktop"));
  EXPECT_TRUE(desktop_.config_error().empty());
}

TEST_F(WishDesktopTest, BuildChromeIsIdempotent) {
  desktop_.build_chrome();
  desktop_.build_chrome();
  desktop_.build_chrome();
  EXPECT_EQ(shell_count(), size_t{1});
}

TEST_F(WishDesktopTest, CustomConfigBuildsTheTree) {
  write_config(kCustomConfig);
  desktop_.build_chrome();

  auto tools = entry("tools");
  ASSERT_TRUE(tools);
  EXPECT_EQ(tools->get_as<std::string>("label"_key, std::string{}), "Tools");
  EXPECT_EQ(tools->get_as<std::string>("tooltip"_key, std::string{}), "Some tools");
  auto ed = entry("ed");
  ASSERT_TRUE(ed);
  EXPECT_EQ(ed->get_as<std::string>("app"_key, std::string{}), "ext/editor");
  EXPECT_EQ(ed->get_as<std::string>("tooltip"_key, std::string{}), "Edits things");
  EXPECT_TRUE(ed->get_as<bool>("pinned"_key, false));
  EXPECT_TRUE(ed->get_as<bool>("on_desktop"_key, false));
  EXPECT_TRUE(entry("test/apps/calc")->get_as<bool>("on_desktop"_key, false));
  EXPECT_FALSE(entry("test/apps/nano")->get_as<bool>("pinned"_key, true));
  auto dark = entry("dark");
  ASSERT_TRUE(dark);
  EXPECT_EQ(dark->get_as<std::string>("app"_key, std::string{"x"}), "");
  EXPECT_TRUE(entry("bye"));
}

TEST_F(WishDesktopTest, InvalidConfigAtStartUsesTheDefaultAndReportsIt) {
  write_config("start_menu:\n  items:\n    - {label: A}\n");
  desktop_.build_chrome();
  EXPECT_NE(
      shell()->get_as<std::string>("config_error"_key, std::string{}).find("start_menu.items[0]"), std::string::npos);
  EXPECT_TRUE(entry("test/apps/calc")); // the default menu
}

// ── Reloading ────────────────────────────────────────────────────────────────

TEST_F(WishDesktopTest, EditingTheFileRebuildsTheShell) {
  desktop_.build_chrome();
  ASSERT_TRUE(entry("test/apps/calc"));
  // A different size, so the change is seen even within one timestamp tick.
  write_config("start_menu:\n  items:\n    - {label: Only, id: only.entry, action: quit}\n");
  ASSERT_TRUE(eventually([&] { return entry("only.entry") != nullptr; }));
  EXPECT_FALSE(entry("test/apps/calc"));
  EXPECT_EQ(shell_count(), size_t{1}); // the old shell is gone
}

TEST_F(WishDesktopTest, InvalidEditKeepsTheMenuAndShowsTheError) {
  write_config(kCustomConfig);
  desktop_.build_chrome();
  write_config("start_menu:\n  items:\n    - {label: Broken, run: {oops: 1}}\n");
  ASSERT_TRUE(eventually([&] {
    auto sh = shell();
    return sh && !sh->get_as<std::string>("config_error"_key, std::string{}).empty();
  }));
  EXPECT_NE(desktop_.config_error().find("start_menu.items[0].run"), std::string::npos);
  EXPECT_TRUE(entry("ed")); // still the previous menu

  // Fixing the file clears the error.
  write_config(kCustomConfig);
  ASSERT_TRUE(eventually([&] {
    auto sh = shell();
    return sh && sh->get_as<std::string>("config_error"_key, std::string{"x"}).empty();
  }));
}

// ── Entries and actions ──────────────────────────────────────────────────────

TEST_F(WishDesktopTest, ActivatingARunEntryStartsItsCommand) {
  write_config(kCustomConfig);
  desktop_.build_chrome();
  dynamic p;
  p["id"_key] = std::string{"ed"};
  emit("activate", std::move(p));

  ASSERT_TRUE(eventually([&] { return !launcher_.requests.empty(); }));
  const auto& req = launcher_.requests.front();
  EXPECT_EQ(req.argv, (std::vector<std::string>{"editor", "--flag", config_file_.string()}));
  EXPECT_EQ(
      req.env, (std::vector<std::pair<std::string, std::string>>{{"WISH_HOST", "127.0.0.1"}, {"EDITOR_MODE", "dark"}}));
  EXPECT_EQ(req.cwd, "/tmp");
}

TEST_F(WishDesktopTest, RegisteredAppsRunAsWishClients) {
  desktop_.build_chrome();
  EXPECT_TRUE(desktop_.launch_entry("test/apps/calc"));
  ASSERT_EQ(launcher_.requests.size(), size_t{1});
  EXPECT_EQ(launcher_.requests[0].argv, (std::vector<std::string>{"wish", "client", "--run=test/apps/calc"}));
}

TEST_F(WishDesktopTest, ActivatingAnActionEntryRunsTheAction) {
  write_config(kCustomConfig);
  desktop_.build_chrome();
  dynamic p;
  p["id"_key] = std::string{"bye"};
  emit("activate", std::move(p));
  // wait_for_quit() blocks until request_quit() fires.
  desktop_.wait_for_quit();
}

TEST_F(WishDesktopTest, SetThemeActionAppliesAndPersists) {
  write_config(kCustomConfig);
  desktop_.build_chrome();
  ASSERT_TRUE(desktop_.launch_entry("dark"));
  EXPECT_EQ(desktop_.settings().theme, "dark");
  auto saved = session().user_store->get("wish.desktop");
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->get_as<std::string>("theme"_key, std::string{}), "dark");
}

TEST_F(WishDesktopTest, ShutdownServerActionFlagsTheShell) {
  desktop_.build_chrome();
  desktop_.run_action("shutdown_server", dynamic{});
  EXPECT_TRUE(shell()->get_as<bool>("shutdown"_key, false));
}

TEST_F(WishDesktopTest, LaunchingUnknownOrNonRunnableEntriesFails) {
  write_config(kCustomConfig);
  desktop_.build_chrome();
  EXPECT_FALSE(desktop_.launch_entry("no.such.entry"));
  EXPECT_FALSE(desktop_.launch_entry("tools")); // a folder
  desktop_.set_launcher(nullptr);
  EXPECT_FALSE(desktop_.launch_entry("ed"));
  EXPECT_THROW(desktop_.run_action("explode", dynamic{}), std::runtime_error);
}

// ── Pins, desktop icons, theme ───────────────────────────────────────────────

TEST_F(WishDesktopTest, PinEventUpdatesTheEntryAndPersists) {
  write_config(kCustomConfig);
  desktop_.build_chrome();
  dynamic p;
  p["id"_key] = std::string{"test/apps/nano"};
  p["pinned"_key] = true;
  emit("pin", std::move(p));

  ASSERT_TRUE(eventually([&] { return entry("test/apps/nano")->get_as<bool>("pinned"_key, false); }));
  // The file's pins are the starting point of the user's own.
  auto pinned = desktop_.settings().pinned;
  EXPECT_EQ(pinned, (std::vector<std::string>{"ed", "test/apps/nano"}));

  auto saved = session().user_store->get("wish.desktop");
  ASSERT_TRUE(saved.has_value());
  const field* list = saved->findField("pinned"_key);
  ASSERT_TRUE(list && list->is<dynamic_ptr>());
  std::vector<std::string> stored;
  list->as<dynamic_ptr>()->forEach([&](bdg::bison::key_t, const field& v) {
    if (v.is<std::string>())
      stored.push_back(v.as<std::string>());
  });
  EXPECT_EQ(stored, pinned);
}

TEST_F(WishDesktopTest, DesktopIconEventUpdatesTheEntry) {
  write_config(kCustomConfig);
  desktop_.build_chrome();
  dynamic p;
  p["id"_key] = std::string{"ed"};
  p["on_desktop"_key] = false;
  emit("desktop_icon", std::move(p));

  ASSERT_TRUE(eventually([&] { return !entry("ed")->get_as<bool>("on_desktop"_key, true); }));
  EXPECT_EQ(desktop_.settings().on_desktop, std::vector<std::string>{"test/apps/calc"});
}

TEST_F(WishDesktopTest, SavedPinsWinOverTheFile) {
  dynamic saved;
  auto pinned = std::make_shared<dynamic>();
  (*pinned)[bdg::bison::key_t{0u}] = std::string{"test/apps/calc"};
  saved["pinned"_key] = dynamic_ptr{pinned};
  saved["desktop"_key] = dynamic_ptr{std::make_shared<dynamic>()};
  saved["theme"_key] = std::string{"light"};
  // The session exists once the bridge connected upstream (start()).
  session().user_store->set("wish.desktop", saved);
  write_config(kCustomConfig);

  desktop_.build_chrome();

  EXPECT_TRUE(entry("test/apps/calc")->get_as<bool>("pinned"_key, false));
  EXPECT_FALSE(entry("ed")->get_as<bool>("pinned"_key, true));
  EXPECT_FALSE(entry("ed")->get_as<bool>("on_desktop"_key, true));
  EXPECT_EQ(desktop_.settings().theme, "light");
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

// ── Appearance and taskbar settings ──────────────────────────────────────────

namespace {

std::string read_file(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file(const std::filesystem::path& file, const std::string& bytes) {
  std::filesystem::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
}

} // namespace

TEST_F(WishDesktopTest, AppearanceAndTaskbarSettingsReachTheShell) {
  write_config(
      "appearance:\n"
      "  wallpaper_fit: tile\n"
      "  background: \"#1E3A5F\"\n"
      "  desktop_icons: false\n"
      "taskbar:\n"
      "  time_format: \"%I:%M %p\"\n"
      "  date_format: \"%A\"\n");
  desktop_.build_chrome();

  auto sh = shell();
  ASSERT_NE(sh, nullptr);
  EXPECT_EQ(sh->get_as<std::string>("wallpaper_fit"_key, std::string{}), "tile");
  EXPECT_EQ(sh->get_as<std::string>("background"_key, std::string{}), "#1E3A5F");
  EXPECT_FALSE(sh->get_as<bool>("show_icons"_key, true));
  EXPECT_EQ(sh->get_as<std::string>("time_format"_key, std::string{}), "%I:%M %p");
  EXPECT_EQ(sh->get_as<std::string>("date_format"_key, std::string{}), "%A");
  EXPECT_TRUE(sh->get_as<std::string>("wallpaper"_key, std::string{"x"}).empty());
  EXPECT_TRUE(sh->get_as<std::string>("config_error"_key, std::string{"x"}).empty());
}

TEST_F(WishDesktopTest, EditingAppearanceRebuildsTheShell) {
  desktop_.build_chrome();
  ASSERT_EQ(shell()->get_as<std::string>("wallpaper_fit"_key, std::string{}), "cover");
  write_config("appearance:\n  wallpaper_fit: contain\n");
  ASSERT_TRUE(eventually([&] {
    auto sh = shell();
    return sh && sh->get_as<std::string>("wallpaper_fit"_key, std::string{}) == "contain";
  }));
  EXPECT_EQ(shell_count(), size_t{1});
}

TEST_F(WishDesktopTest, ConfigWallpaperIsUploadedAndReplacedWhenTheImageChanges) {
  // Relative to the config file's directory.
  auto image = config_file_.parent_path() / "walls" / "a.png";
  write_file(image, "one");
  write_config("appearance:\n  wallpaper: walls/a.png\n");
  desktop_.build_chrome();

  std::string first = shell()->get_as<std::string>("wallpaper"_key, std::string{});
  ASSERT_EQ(first.rfind("private/apps/wish.desktop/wallpaper_", 0), size_t{0}) << first;
  EXPECT_EQ(first.substr(first.size() - 4), ".png");
  EXPECT_EQ(read_file(session().resource_dir / first), "one");

  // A new image in the same file: uploaded under a new name, the old one
  // erased.
  write_file(image, "second image");
  std::string second;
  ASSERT_TRUE(eventually([&] {
    auto sh = shell();
    second = sh ? sh->get_as<std::string>("wallpaper"_key, std::string{}) : std::string{};
    return !second.empty() && second != first;
  }));
  EXPECT_EQ(read_file(session().resource_dir / second), "second image");
  EXPECT_FALSE(std::filesystem::exists(session().resource_dir / first));
}

TEST_F(WishDesktopTest, MissingWallpaperIsReported) {
  write_config("appearance:\n  wallpaper: nowhere.png\n");
  desktop_.build_chrome();
  auto sh = shell();
  ASSERT_NE(sh, nullptr);
  EXPECT_TRUE(sh->get_as<std::string>("wallpaper"_key, std::string{"x"}).empty());
  EXPECT_NE(
      sh->get_as<std::string>("config_error"_key, std::string{}).find("appearance.wallpaper: cannot read"),
      std::string::npos);

  // The image appears: the error goes away.
  write_file(config_file_.parent_path() / "nowhere.png", "img");
  ASSERT_TRUE(eventually([&] {
    auto s2 = shell();
    return s2 && !s2->get_as<std::string>("wallpaper"_key, std::string{}).empty() &&
        s2->get_as<std::string>("config_error"_key, std::string{"x"}).empty();
  }));
}

TEST_F(WishDesktopTest, WallpaperFlagWinsOverTheConfig) {
  write_file(store_dir_ / "flag.jpg", "flag");
  write_file(config_file_.parent_path() / "file.png", "file");
  write_config("appearance:\n  wallpaper: file.png\n");
  desktop_.set_wallpaper(store_dir_ / "flag.jpg");
  desktop_.build_chrome();
  std::string src = shell()->get_as<std::string>("wallpaper"_key, std::string{});
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(read_file(session().resource_dir / src), "flag");
}

TEST_F(WishDesktopTest, ConfigThemeWinsOverTheSavedOneAndAppliesWhenEdited) {
  dynamic saved;
  saved["theme"_key] = std::string{"light"};
  session().user_store->set("wish.desktop", saved);
  write_config("appearance:\n  theme: dark\n");
  desktop_.build_chrome();
  EXPECT_EQ(desktop_.settings().theme, "dark");

  // The tray toggle still works ...
  dynamic p;
  p["preset"_key] = std::string{"light"};
  emit("theme", std::move(p));
  ASSERT_TRUE(eventually([&] { return desktop_.settings().theme == "light"; }));

  // ... and an edit that leaves the theme alone keeps the toggle's choice.
  write_config("appearance:\n  theme: dark\ntaskbar:\n  date_format: \"%A\"\n");
  ASSERT_TRUE(eventually([&] {
    auto sh = shell();
    return sh && sh->get_as<std::string>("date_format"_key, std::string{}) == "%A";
  }));
  EXPECT_EQ(desktop_.settings().theme, "light");

  // Changing it in the file applies it.
  write_config("appearance:\n  theme: classic\n");
  ASSERT_TRUE(eventually([&] { return desktop_.settings().theme == "classic"; }));
}

TEST_F(WishDesktopTest, NoThemeInTheConfigKeepsTheSavedOne) {
  dynamic saved;
  saved["theme"_key] = std::string{"dark"};
  session().user_store->set("wish.desktop", saved);
  desktop_.build_chrome(); // the default config sets no theme
  EXPECT_EQ(desktop_.settings().theme, "dark");
}

// ── Running apps ─────────────────────────────────────────────────────────────

TEST_F(WishDesktopTest, ConnectedClientIsPublishedOnItsEntry) {
  write_config(kCustomConfig);
  desktop_.build_chrome();

  client app_client(std::make_unique<memory_client_transport>(downstream_transport_.connect()));
  dynamic params;
  params["app"_key] = std::string{"ext/editor"};
  app_client.connect(std::move(params));

  auto sessions_of_ed = [&] {
    auto e = entry("ed");
    const field* f = e ? e->findField("sessions"_key) : nullptr;
    return f && f->is<std::vector<int32_t>>() ? f->as<std::vector<int32_t>>() : std::vector<int32_t>{};
  };
  ASSERT_TRUE(eventually([&] { return sessions_of_ed().size() == 1; }));
  EXPECT_EQ(shell()->get_as<int32_t>("clients"_key, 0), 1);

  app_client.disconnect();
  ASSERT_TRUE(eventually([&] { return sessions_of_ed().empty(); }));
  EXPECT_EQ(shell()->get_as<int32_t>("clients"_key, -1), 0);
}

TEST_F(WishDesktopTest, ShortRunNameMatchesTheRegisteredEntry) {
  desktop_.build_chrome();
  client app_client(std::make_unique<memory_client_transport>(downstream_transport_.connect()));
  dynamic params;
  params["app"_key] = std::string{"nano"};
  app_client.connect(std::move(params));
  ASSERT_TRUE(eventually([&] {
    auto e = entry("test/apps/nano");
    const field* f = e ? e->findField("sessions"_key) : nullptr;
    return f && f->is<std::vector<int32_t>>() && f->as<std::vector<int32_t>>().size() == 1;
  }));
  app_client.disconnect();
}

// ── __WishDesktop, served to downstream clients ──────────────────────────────

TEST_F(WishDesktopTest, WishDesktopObjectIsAnsweredByTheBridge) {
  write_config(kCustomConfig);
  desktop_.build_chrome();

  client app_client(std::make_unique<memory_client_transport>(downstream_transport_.connect()));
  app_client.connect();
  auto desk = app_client.instantiate("wish"_key, "__WishDesktop"_key).get();

  dynamic launch;
  launch["id"_key] = std::string{"ed"};
  auto ok = desk.call("launch"_key, std::move(launch)).get();
  EXPECT_TRUE(ok.get_as<bool>("ok"_key, false));
  ASSERT_EQ(launcher_.requests.size(), size_t{1});
  EXPECT_EQ(launcher_.requests[0].argv.front(), "editor");

  dynamic unknown;
  unknown["id"_key] = std::string{"no.such"};
  EXPECT_FALSE(desk.call("launch"_key, std::move(unknown)).get().get_as<bool>("ok"_key, true));

  auto items = desk.call("items"_key, dynamic{}).get();
  const field* list = items.findField("items"_key);
  ASSERT_TRUE(list && list->is<dynamic_ptr>());
  std::vector<std::string> ids;
  list->as<dynamic_ptr>()->forEach([&](bdg::bison::key_t, const field& v) {
    if (v.is<dynamic_ptr>() && v.as<dynamic_ptr>())
      ids.push_back(v.as<dynamic_ptr>()->get_as<std::string>("id"_key, std::string{}));
  });
  EXPECT_NE(std::find(ids.begin(), ids.end(), "ed"), ids.end());
  EXPECT_NE(std::find(ids.begin(), ids.end(), "bye"), ids.end());

  // Nothing about it reached the upstream server.
  for (const auto& [key, root] : session().top_level_objects)
    EXPECT_NE(root->class_key(), bdg::bison::key_t{"__WishDesktop"});
  app_client.disconnect();
}

TEST_F(WishDesktopTest, UnknownAppGetsAnUnlistedEntryOfItsOwn) {
  desktop_.build_chrome();

  // A client-ABI app the registry doesn't know, identifying itself.
  auto connect = [&] {
    auto c = std::make_unique<client>(std::make_unique<memory_client_transport>(downstream_transport_.connect()));
    dynamic params;
    params["app"_key] = std::string{"ext/py/todo"};
    params["app_label"_key] = std::string{"To-do"};
    params["app_icon"_key] = std::string{"res/icons/check.png"};
    c->connect(std::move(params));
    return c;
  };
  // Top-level DesktopApp objects next to the shell, with their sessions.
  auto extra_apps = [&] {
    std::vector<wish::ui_element_ptr> out;
    for (const auto& [key, root] : session().top_level_objects)
      if (root && root->class_key() == bdg::bison::key_t{"DesktopApp"})
        out.push_back(root);
    return out;
  };
  auto sessions_of = [](const wish::ui_element_ptr& e) {
    const field* f = e->findField("sessions"_key);
    return f && f->is<std::vector<int32_t>>() ? f->as<std::vector<int32_t>>() : std::vector<int32_t>{};
  };

  auto first = connect();
  ASSERT_TRUE(eventually([&] { return extra_apps().size() == 1 && sessions_of(extra_apps()[0]).size() == 1; }));
  auto entry = extra_apps()[0];
  EXPECT_EQ(entry->get_as<std::string>("app"_key, std::string{}), "ext/py/todo");
  EXPECT_EQ(entry->get_as<std::string>("label"_key, std::string{}), "To-do");
  EXPECT_EQ(entry->get_as<std::string>("icon"_key, std::string{}), "res/icons/check.png");
  EXPECT_FALSE(entry->get_as<bool>("listed"_key, true));

  // A second instance of the same app shares the entry.
  auto second = connect();
  ASSERT_TRUE(eventually([&] { return sessions_of(entry).size() == 2; }));
  EXPECT_EQ(extra_apps().size(), size_t{1});
  EXPECT_EQ(shell()->get_as<int32_t>("clients"_key, 0), 2);

  first->disconnect();
  second->disconnect();
  ASSERT_TRUE(eventually([&] { return sessions_of(entry).empty(); }));
}

TEST_F(WishDesktopTest, UnknownAppWithoutLabelUsesTheLastPathSegment) {
  desktop_.build_chrome();
  client c(std::make_unique<memory_client_transport>(downstream_transport_.connect()));
  dynamic params;
  params["app"_key] = std::string{"ext/py/stopwatch"};
  c.connect(std::move(params));

  wish::ui_element_ptr entry;
  ASSERT_TRUE(eventually([&] {
    for (const auto& [key, root] : session().top_level_objects)
      if (root && root->class_key() == bdg::bison::key_t{"DesktopApp"})
        entry = root;
    return entry != nullptr;
  }));
  EXPECT_EQ(entry->get_as<std::string>("label"_key, std::string{}), "stopwatch");
  c.disconnect();
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

// ── multi_server_transport ───────────────────────────────────────────────────

TEST(MultiServerTransportTest, AcceptsFromEveryTransport) {
  memory_server_transport primary;
  auto extra_owned = std::make_unique<memory_server_transport>();
  memory_server_transport& extra = *extra_owned;
  wish::multi_server_transport multi{primary};
  multi.add(std::move(extra_owned));
  multi.start(dynamic{});

  memory_client_transport via_extra{extra.connect()};
  via_extra.open(dynamic{});
  EXPECT_NE(multi.accept(std::chrono::seconds{2}), nullptr);

  memory_client_transport via_primary{primary.connect()};
  via_primary.open(dynamic{});
  EXPECT_NE(multi.accept(std::chrono::seconds{2}), nullptr);

  EXPECT_EQ(multi.accept(std::chrono::milliseconds{20}), nullptr);
  multi.stop();
}

TEST(MultiServerTransportTest, StopEndsAcceptAndIsIdempotent) {
  memory_server_transport primary;
  wish::multi_server_transport multi{primary};
  multi.start(dynamic{});
  multi.stop();
  const auto before = std::chrono::steady_clock::now();
  EXPECT_EQ(multi.accept(std::chrono::seconds{2}), nullptr);
  EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds{1});
  multi.stop();
}

// ── Fixed endpoint / single instance ─────────────────────────────────────────

namespace {

// A pipe name no other test (or real desktop) uses: ctest runs tests as
// parallel processes.
std::string unique_endpoint_name() {
  const std::string id = std::to_string(bdg::bison::rmi::shared::generate_id().id);
#if defined(_WIN32) || defined(__CYGWIN__)
  return "\\\\.\\pipe\\wish_desktop_test_" + id;
#else
  return "@wish_desktop_test_" + id;
#endif
}

} // namespace

TEST(DesktopEndpointClaimTest, SecondClaimFailsUntilTheFirstIsReleased) {
  const std::string name = unique_endpoint_name();
  auto first = wish::claim_desktop_endpoint(name);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(wish::claim_desktop_endpoint(name), nullptr);

  first->stop();
  first.reset();
  auto again = wish::claim_desktop_endpoint(name);
  EXPECT_NE(again, nullptr);
}

// A wish_desktop serving its downstream transport and the fixed endpoint
// together, as wish_desktop_app::make_bridge() sets it up.
class WishDesktopEndpointTest : public ::testing::Test {
 protected:
  std::filesystem::path store_dir_ = test_store_dir();
  std::string endpoint_name_ = unique_endpoint_name();
  memory_server_transport upstream_transport_;
  tracking_upstream_server upstream_srv_{upstream_transport_, std::make_unique<wish::null_renderer>()};

  memory_server_transport primary_transport_;
  wish::multi_server_transport downstream_{primary_transport_};
  wish::wish_desktop desktop_{
      downstream_,
      std::make_unique<memory_client_transport>(upstream_transport_.connect()),
      upstream_params()};
  fake_launcher launcher_;

  void SetUp() override {
    ASSERT_TRUE(kAppsRegistered);
    auto endpoint = wish::claim_desktop_endpoint(endpoint_name_);
    ASSERT_NE(endpoint, nullptr);
    downstream_.add(std::move(endpoint));
    upstream_srv_.set_store_dir(store_dir_);
    upstream_srv_.start(std::make_shared<wish::local_auth_module>());
    desktop_.set_launcher(&launcher_);
    desktop_.set_client_command({"wish", "client"});
    desktop_.set_config_file(store_dir_ / "desktop" / "desktop.yaml");
    desktop_.start();
    desktop_.build_chrome();
  }

  void TearDown() override {
    desktop_.stop_tasks();
    desktop_.stop();
    upstream_srv_.stop();
    std::error_code ec;
    std::filesystem::remove_all(store_dir_, ec);
  }
};

TEST_F(WishDesktopEndpointTest, ClientOnTheEndpointGetsADesktopSession) {
  client app_client(std::make_unique<named_pipe_client_transport>(endpoint_name_));
  app_client.connect();
  auto desk = app_client.instantiate("wish"_key, "__WishDesktop"_key).get();
  auto items = desk.call("items"_key, dynamic{}).get();
  EXPECT_NE(items.findField("items"_key), nullptr);
  app_client.disconnect();
}

TEST_F(WishDesktopEndpointTest, EndpointStaysClaimedWhileTheDesktopRuns) {
  EXPECT_EQ(wish::claim_desktop_endpoint(endpoint_name_), nullptr);
}

TEST_F(WishDesktopEndpointTest, StoppingTheDesktopReleasesTheEndpoint) {
  desktop_.stop_tasks();
  desktop_.stop();
  EXPECT_NE(wish::claim_desktop_endpoint(endpoint_name_), nullptr);
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
