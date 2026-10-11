// MIT License © 2026 Binary Dice Games
// Tests for app/wish_cli/desktop/desktop_config.hpp -- the `wish desktop`
// configuration file and its sections.
#include "app/wish_cli/desktop/desktop_config.hpp"

#include "src/client/app_registry.hpp"
#include "src/rmi/shared/ids.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace wish = bdg::wish;
namespace dc = bdg::wish::desktop;

namespace {

// Three fake apps in two collections -- the whole registry of this test.
const bool kAppsRegistered = [] {
  for (auto [name, collection] : {std::pair{"nano", "apps"}, std::pair{"calc", "apps"}, std::pair{"git", "dev"}}) {
    wish::register_app(
        {.name = name,
         .organization = "test",
         .collection = collection,
         .description = std::string{"Fake "} + name,
         .icon = std::string{"res/icons/"} + name + ".png",
         .params = {},
         .run = [](wish::wish_app_host&) {}});
  }
  return true;
}();

dc::placeholders ph() {
  return {{"/bin/wish", "client"}, "/home/u/.wish/desktop/desktop.yaml", "/home/u"};
}

dc::start_menu parse(const std::string& text) {
  return dc::parse_config_text(text, ph()).menu;
}

std::string error_of(const std::string& text) {
  try {
    parse(text);
  } catch (const std::exception& ex) {
    return ex.what();
  }
  return {};
}

std::filesystem::path temp_file() {
  return std::filesystem::temp_directory_path() /
      ("wish_desktop_config_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_" +
       std::to_string(bdg::bison::rmi::shared::generate_id().id)) /
      "desktop.yaml";
}

} // namespace

// ── Entries ──────────────────────────────────────────────────────────────────

TEST(DesktopConfig, ParsesFoldersRunActionAndSeparator) {
  ASSERT_TRUE(kAppsRegistered);
  auto m = parse(R"(
start_menu:
  items:
    - label: Python apps
      icon: res/icons/folder.png
      items:
        - label: To-do
          id: py.todo
          icon: res/icons/check.png
          tooltip: A to-do list
          run: [python3, "{home}/todo.py"]
          app: ext/todo
          env: {LEVEL: 3}
          cwd: "{home}"
    - separator: true
    - label: Quit
      action: set_theme
      args: {preset: dark}
)");
  ASSERT_EQ(m.items.size(), size_t{3});
  const auto& folder = m.items[0];
  EXPECT_EQ(folder.kind, dc::entry_kind::folder);
  EXPECT_EQ(folder.id, "python-apps");
  ASSERT_EQ(folder.children.size(), size_t{1});
  const auto& todo = folder.children[0];
  EXPECT_EQ(todo.kind, dc::entry_kind::run);
  EXPECT_EQ(todo.id, "py.todo");
  EXPECT_EQ(todo.tooltip, "A to-do list");
  EXPECT_EQ(todo.argv, (std::vector<std::string>{"python3", "/home/u/todo.py"}));
  EXPECT_EQ(todo.app, "ext/todo");
  EXPECT_EQ(todo.cwd, "/home/u");
  EXPECT_EQ(todo.env, (std::vector<std::pair<std::string, std::string>>{{"LEVEL", "3"}}));
  EXPECT_EQ(m.items[1].kind, dc::entry_kind::separator);
  EXPECT_EQ(m.items[2].kind, dc::entry_kind::action);
  EXPECT_EQ(m.items[2].action, "set_theme");
  EXPECT_EQ(m.items[2].args["preset"], "dark");
}

TEST(DesktopConfig, WishClientPlaceholderAndDefaultApp) {
  auto m = parse(R"(
start_menu:
  items:
    - label: Edit
      run: "{wish_client} --run=bdg/desktop/nano -- '{config_file}'"
)");
  ASSERT_EQ(m.items.size(), size_t{1});
  EXPECT_EQ(
      m.items[0].argv,
      (std::vector<std::string>{
          "/bin/wish", "client", "--run=bdg/desktop/nano", "--", "/home/u/.wish/desktop/desktop.yaml"}));
  EXPECT_EQ(m.items[0].app, "bdg/desktop/nano");
}

TEST(DesktopConfig, AppsAllMakesAFolderPerCollection) {
  auto m = parse("start_menu:\n  items:\n    - apps: all\n");
  ASSERT_EQ(m.items.size(), size_t{2}); // "apps" and "dev"
  EXPECT_EQ(m.items[0].label, "Apps");
  ASSERT_EQ(m.items[0].children.size(), size_t{2});
  const auto& calc = m.items[0].children[0];
  EXPECT_EQ(calc.id, "test/apps/calc");
  EXPECT_EQ(calc.app, "test/apps/calc");
  EXPECT_EQ(calc.icon, "res/icons/calc.png");
  EXPECT_EQ(calc.tooltip, "Fake calc");
  EXPECT_EQ(calc.argv, (std::vector<std::string>{"/bin/wish", "client", "--run=test/apps/calc"}));
  EXPECT_EQ(m.items[1].label, "Dev");
}

TEST(DesktopConfig, AppsOfOneCollectionAreInline) {
  auto m = parse("start_menu:\n  items:\n    - apps: dev\n");
  ASSERT_EQ(m.items.size(), size_t{1});
  EXPECT_EQ(m.items[0].id, "test/dev/git");
}

TEST(DesktopConfig, DerivedIdsAreUnique) {
  auto m = parse(R"(
start_menu:
  items:
    - {label: Same, action: quit}
    - {label: Same, action: quit}
)");
  EXPECT_EQ(m.items[0].id, "same");
  EXPECT_EQ(m.items[1].id, "same-2");
}

TEST(DesktopConfig, PinnedAndDesktopListsAndFlags) {
  auto m = parse(R"(
start_menu:
  pinned: [test/apps/nano, no/such/app]
  desktop: [test/dev/git]
  items:
    - apps: all
    - {label: Quit, action: quit, pinned: true}
)");
  EXPECT_TRUE(m.find("test/apps/nano")->pinned);
  EXPECT_FALSE(m.find("test/apps/calc")->pinned);
  EXPECT_TRUE(m.find("test/dev/git")->on_desktop);
  EXPECT_TRUE(m.find("quit")->pinned);
}

TEST(DesktopConfig, PowerSection) {
  auto m = parse("start_menu:\n  power:\n    - {label: Quit desktop, action: quit}\n");
  ASSERT_EQ(m.power.size(), size_t{1});
  EXPECT_EQ(m.power[0].id, "power/quit-desktop");
  EXPECT_EQ(m.find("power/quit-desktop"), &m.power[0]);
}

TEST(DesktopConfig, JsonIsAccepted) {
  auto m = parse(R"({"start_menu": {"items": [{"label": "Top", "run": ["top"]}]}})");
  ASSERT_EQ(m.items.size(), size_t{1});
  EXPECT_EQ(m.items[0].argv, std::vector<std::string>{"top"});
}

TEST(DesktopConfig, SplitCommandHonorsQuotes) {
  EXPECT_EQ(dc::split_command(R"(a "b c" 'd e' f)"), (std::vector<std::string>{"a", "b c", "d e", "f"}));
  EXPECT_EQ(dc::split_command(R"(x "")"), (std::vector<std::string>{"x", ""}));
}

// ── Errors ───────────────────────────────────────────────────────────────────

TEST(DesktopConfig, ErrorsNameTheirPath) {
  EXPECT_NE(
      error_of("start_menu:\n  items:\n    - {label: A, run: 3.5e100x}\n    - {label: B, run: {x: 1}}\n")
          .find("start_menu.items[1].run: expected a list or a string"),
      std::string::npos);
  EXPECT_NE(
      error_of("start_menu:\n  items:\n    - {label: A, action: explode}\n").find("unknown action 'explode'"),
      std::string::npos);
  EXPECT_NE(
      error_of("start_menu:\n  items:\n    - {label: A}\n").find("start_menu.items[0]: an entry needs exactly one"),
      std::string::npos);
  EXPECT_NE(
      error_of("start_menu:\n  items:\n    - {run: [x]}\n").find("start_menu.items[0].label: missing"),
      std::string::npos);
  EXPECT_NE(
      error_of("start_menu:\n  items:\n    - {label: A, id: x, action: quit}\n    - {label: B, id: x, action: quit}\n")
          .find("duplicate id 'x'"),
      std::string::npos);
  EXPECT_NE(error_of("start_menu: [1, 2]\n").find("start_menu: expected a map"), std::string::npos);
  EXPECT_NE(error_of("start_menu:\n  items: [\n").find("line"), std::string::npos); // YAML syntax error
}

// ── Whole file ───────────────────────────────────────────────────────────────

TEST(DesktopConfig, DefaultConfigParses) {
  auto m = parse(dc::default_config_text());
  EXPECT_FALSE(m.items.empty());
  ASSERT_EQ(m.power.size(), size_t{2});
  EXPECT_EQ(m.power[0].action, "quit");
}

TEST(DesktopConfig, DefaultMenuHasAPinnedSettingsEntry) {
  auto m = parse(dc::default_config_text());
  const dc::menu_entry* settings = m.find("settings");
  ASSERT_NE(settings, nullptr);
  EXPECT_EQ(settings->kind, dc::entry_kind::run);
  EXPECT_EQ(settings->app, "bdg/desktop/conf");
  EXPECT_EQ(settings->argv, (std::vector<std::string>{"/bin/wish", "client", "--run=bdg/desktop/conf"}));
  EXPECT_TRUE(settings->pinned);
}

TEST(DesktopConfig, NoStartMenuSectionUsesTheDefaultMenu) {
  auto m = parse("version: 1\nwallpaper: {file: x.png}\n");
  EXPECT_EQ(m.power.size(), size_t{2});
}

// ── appearance and taskbar ───────────────────────────────────────────────────

TEST(DesktopConfig, MissingSectionsKeepTheirDefaults) {
  auto c = dc::parse_config_text("version: 1\n", ph());
  EXPECT_TRUE(c.look.wallpaper.empty());
  EXPECT_EQ(c.look.wallpaper_fit, "cover");
  EXPECT_TRUE(c.look.background.empty());
  EXPECT_TRUE(c.look.theme.empty());
  EXPECT_TRUE(c.look.desktop_icons);
  EXPECT_EQ(c.taskbar.time_format, "%H:%M");
  EXPECT_EQ(c.taskbar.date_format, "%d/%m/%Y");
}

TEST(DesktopConfig, DefaultConfigHasDefaultAppearance) {
  auto c = dc::parse_config_text(dc::default_config_text(), ph());
  EXPECT_TRUE(c.look.wallpaper.empty());
  EXPECT_EQ(c.look.wallpaper_fit, "cover");
  EXPECT_TRUE(c.look.theme.empty()); // the tray toggle's choice is kept
  EXPECT_TRUE(c.look.desktop_icons);
  EXPECT_EQ(c.taskbar.time_format, "%H:%M");
}

TEST(DesktopConfig, AppearanceAndTaskbarSettings) {
  auto c = dc::parse_config_text(
      "appearance:\n"
      "  wallpaper: /pics/beach.jpg\n"
      "  wallpaper_fit: tile\n"
      "  background: \"#1E3A5F\"\n"
      "  theme: dark\n"
      "  desktop_icons: false\n"
      "  future_setting: 1\n" // ignored
      "taskbar:\n"
      "  time_format: \"%I:%M %p\"\n"
      "  date_format: \"%A\"\n",
      ph());
  EXPECT_EQ(c.look.wallpaper, std::filesystem::path("/pics/beach.jpg"));
  EXPECT_EQ(c.look.wallpaper_fit, "tile");
  EXPECT_EQ(c.look.background, "#1E3A5F");
  EXPECT_EQ(c.look.theme, "dark");
  EXPECT_FALSE(c.look.desktop_icons);
  EXPECT_EQ(c.taskbar.time_format, "%I:%M %p");
  EXPECT_EQ(c.taskbar.date_format, "%A");
}

TEST(DesktopConfig, WallpaperPathsExpand) {
  auto wallpaper = [](const std::string& value) {
    return dc::parse_config_text("appearance:\n  wallpaper: \"" + value + "\"\n", ph()).look.wallpaper;
  };
  EXPECT_EQ(wallpaper("~/Pictures/a.png"), std::filesystem::path("/home/u/Pictures/a.png"));
  EXPECT_EQ(wallpaper("{home}/b.png"), std::filesystem::path("/home/u/b.png"));
  // Relative to the config file's directory.
  EXPECT_EQ(wallpaper("walls/c.png"), std::filesystem::path("/home/u/.wish/desktop/walls/c.png"));
  EXPECT_EQ(wallpaper("../d.png"), std::filesystem::path("/home/u/.wish/d.png"));
}

TEST(DesktopConfig, AppearanceErrorsNameTheSetting) {
  auto error = [](const std::string& text) {
    try {
      dc::parse_config_text(text, ph());
    } catch (const std::exception& ex) {
      return std::string(ex.what());
    }
    return std::string{};
  };
  EXPECT_NE(
      error("appearance:\n  wallpaper_fit: zoom\n").find("appearance.wallpaper_fit: expected cover"),
      std::string::npos);
  EXPECT_NE(
      error("appearance:\n  background: blue\n").find("appearance.background: expected a color"), std::string::npos);
  EXPECT_NE(error("appearance:\n  background: \"#12345\"\n").find("appearance.background"), std::string::npos);
  EXPECT_NE(
      error("appearance:\n  desktop_icons: maybe\n").find("appearance.desktop_icons: expected true or false"),
      std::string::npos);
  EXPECT_NE(error("appearance: [1]\n").find("appearance: expected a map"), std::string::npos);
  EXPECT_NE(error("taskbar: 3\n").find("taskbar: expected a map"), std::string::npos);
}

TEST(DesktopConfig, MissingFileGetsTheDefaultWritten) {
  auto file = temp_file();
  auto loaded = dc::load_config(file, ph());
  EXPECT_TRUE(loaded.wrote_default);
  EXPECT_TRUE(loaded.error.empty());
  std::ifstream in(file);
  std::stringstream ss;
  ss << in.rdbuf();
  EXPECT_EQ(ss.str(), dc::default_config_text());
  std::filesystem::remove_all(file.parent_path());
}

TEST(DesktopConfig, BrokenFileFallsBackToTheDefaultAndReportsIt) {
  auto file = temp_file();
  std::filesystem::create_directories(file.parent_path());
  std::ofstream(file) << "start_menu:\n  items:\n    - {label: A}\n";
  auto loaded = dc::load_config(file, ph());
  EXPECT_FALSE(loaded.wrote_default);
  EXPECT_NE(loaded.error.find("desktop.yaml: start_menu.items[0]"), std::string::npos);
  EXPECT_EQ(loaded.config.menu.power.size(), size_t{2}); // the default menu
  std::ifstream in(file);
  std::stringstream ss;
  ss << in.rdbuf();
  EXPECT_EQ(ss.str(), "start_menu:\n  items:\n    - {label: A}\n"); // untouched
  std::filesystem::remove_all(file.parent_path());
}
