// MIT License © 2026 Binary Dice Games
// Tests for app/wish_cli/desktop/desktop_config.hpp -- the `wish desktop`
// configuration file and its start_menu section.
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
  return dc::parse_config_text(text, ph());
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

TEST(DesktopConfig, NoStartMenuSectionUsesTheDefaultMenu) {
  auto m = parse("version: 1\nwallpaper: {file: x.png}\n");
  EXPECT_EQ(m.power.size(), size_t{2});
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
  EXPECT_EQ(loaded.menu.power.size(), size_t{2}); // the default menu
  std::ifstream in(file);
  std::stringstream ss;
  ss << in.rdbuf();
  EXPECT_EQ(ss.str(), "start_menu:\n  items:\n    - {label: A}\n"); // untouched
  std::filesystem::remove_all(file.parent_path());
}
