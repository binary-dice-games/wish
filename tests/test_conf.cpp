// MIT License © 2026 Binary Dice Games
// Tests for the conf module's server form (modules/bdg/desktop/conf): the
// System Settings-like editor built from settings metadata.
#include <gtest/gtest.h>

#include <auth/local_auth_module.hpp>
#include <context/context.hpp>
#include <context/settings_store.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/shared/ids.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
namespace st = bdg::wish::settings;
using namespace bdg::bison::rmi::transport;

namespace {

class capturing_server : public wish::server {
 public:
  using wish::server::server;
  wish::context* last_session{nullptr};

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
  }
};

st::setting_info make(const std::string& name, st::setting_type type, const std::string& category) {
  st::setting_info s;
  s.name = name;
  s.label = st::parse_name(name).field;
  s.type = type;
  s.category = category;
  return s;
}

// The example app's settings, one of each editor type.
void describe_example(wish::persistent_store& store) {
  st::describe_category(store, {"example", "Example", "Demo", "res/icons/palette.png", "#AF52DE", 10});
  auto on = make("bdg.desktop.example.enabled", st::setting_type::boolean, "example");
  on.default_value = true;
  on.section = "Banner";
  st::describe(store, on);
  auto color = make("bdg.desktop.example.color", st::setting_type::color, "example");
  color.default_value = std::string{"#2F7BF5"};
  color.section = "Banner";
  st::describe(store, color);
  auto size = make("bdg.desktop.example.size", st::setting_type::integer, "example");
  size.min = 10.0f;
  size.max = 40.0f;
  size.default_value = int32_t{18};
  st::describe(store, size);
  auto mood = make("bdg.desktop.example.mood", st::setting_type::choice, "example");
  mood.options = {{"happy", "Happy"}, {"calm", "Calm"}};
  mood.default_value = std::string{"happy"};
  st::describe(store, mood);
  auto msg = make("bdg.desktop.example.message", st::setting_type::text, "example");
  msg.description = "Banner text";
  st::describe(store, msg);
  auto speed = make("bdg.desktop.example.speed", st::setting_type::real, "example");
  speed.min = 0.5f;
  speed.max = 3.0f;
  speed.default_value = 1.0f;
  st::describe(store, speed);
}

// A server-wide setting in a built-in category.
void describe_server_theme(wish::persistent_store& store) {
  auto theme = make("wish.cli.server.theme", st::setting_type::choice, "appearance");
  theme.options = {{"light", "Light"}, {"dark", "Dark"}};
  theme.default_value = std::string{"light"};
  theme.restart = true;
  st::describe(store, theme);
}

class ConfTest : public ::testing::Test {
 protected:
  std::filesystem::path dir_ = std::filesystem::temp_directory_path() /
      ("wish_conf_test_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_" +
       std::to_string(bdg::bison::rmi::shared::generate_id().id));
  memory_server_transport transport_;
  std::unique_ptr<capturing_server> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
  wish::ui_root* handler_{nullptr};

  void SetUp() override {
    wish::register_all();
    {
      wish::persistent_store user(dir_ / "users" / "alice.bison");
      describe_example(user);
      wish::persistent_store server(dir_ / "server_store.bison");
      describe_server_theme(server);
    }
  }

  void start(std::set<std::string> admins = {}, const std::string& user = "alice") {
    srv_ = std::make_unique<capturing_server>(transport_, std::make_unique<wish::null_renderer>());
    srv_->set_store_dir(dir_);
    srv_->set_settings_admins(std::move(admins));
    srv_->start(std::make_shared<wish::local_auth_module>());
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    dynamic params;
    if (!user.empty())
      params["username"_key] = user;
    client_->connect(std::move(params));
    proxy_.emplace(client_->instantiate("wish"_key, "Conf"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = "__conf_0";
    auto it = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(it, srv_->last_session->top_level_handlers.end());
    handler_ = it->second;
    ASSERT_NE(handler_, nullptr);
  }

  void TearDown() override {
    proxy_.reset();
    if (client_)
      client_->disconnect();
    client_.reset();
    if (srv_)
      srv_->stop();
    srv_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  /// Every registered element whose path starts with @p prefix (relative
  /// to the form root), as {path, element}, sorted by path.
  std::vector<std::pair<std::string, dynamic_ptr>> elements(const std::string& prefix) {
    std::vector<std::pair<std::string, dynamic_ptr>> out;
    const std::string full = root_ + "." + prefix;
    for (const auto& [id, obj] : srv_->last_session->objects) {
      if (!obj)
        continue;
      const field* p = obj->findField("__path__"_key);
      if (p && p->is<std::string>() && p->as<std::string>().rfind(full, 0) == 0)
        out.emplace_back(p->as<std::string>().substr(root_.size() + 1), obj);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
  }

  /// Elements of class @p cls under @p prefix.
  std::vector<std::pair<std::string, dynamic_ptr>> of_class(const std::string& prefix, const char* cls) {
    std::vector<std::pair<std::string, dynamic_ptr>> out;
    for (auto& e : elements(prefix)) {
      const field* c = e.second->findField(dynamic::CLASS);
      if (c && c->is<bison::key_t>() && c->as<bison::key_t>() == bison::key_t{cls})
        out.push_back(e);
    }
    return out;
  }

  std::vector<std::string> sidebar_labels() {
    std::vector<std::string> labels;
    for (auto& [path, el] : of_class("sidebar.c", "Label"))
      labels.push_back(el->get_as<std::string>("text"_key, std::string{}));
    return labels;
  }

  bison::key_t id_of(const dynamic_ptr& el) {
    return el->get_as<bison::key_t>("__wish_id"_key, bison::key_t{});
  }

  /// Delivers a widget event the way the render loop does.
  void send(const dynamic_ptr& el, const char* event, dynamic payload) {
    handler_->on_event(id_of(el), bison::key_t{event}, payload);
  }

  void select(const std::string& id) {
    dynamic args;
    args["id"_key] = id;
    proxy_->call("select"_key, std::move(args)).get();
  }

  st::setting_value user_value(const std::string& field, st::setting_type type) {
    auto entry = srv_->last_session->user_store->get("bdg.desktop.example");
    return entry ? st::read_value(*entry, field, type) : st::setting_value{};
  }
};

} // namespace

// ── Sidebar ──────────────────────────────────────────────────────────────────

TEST_F(ConfTest, SidebarListsCategoriesThatHaveSettings) {
  start();
  // General is always shown; Appearance holds the server theme; Desktop and
  // Server are empty; then the declared categories: Example and the
  // modules' registered ones (bc, top, ...).
  auto labels = sidebar_labels();
  ASSERT_GE(labels.size(), size_t{3});
  EXPECT_EQ(labels[0], "General");
  EXPECT_EQ(labels[1], "Appearance");
  auto at = [&](const std::string& l) { return std::find(labels.begin(), labels.end(), l) - labels.begin(); };
  // (sidebar_labels() is in path order, not display order.)
  const auto end = static_cast<ptrdiff_t>(labels.size());
  for (const char* l : {"Example", "bc", "top", "tail", "nano", "mc", "zip"})
    EXPECT_LT(at(l), end) << l;
  EXPECT_EQ(at("Desktop"), static_cast<ptrdiff_t>(labels.size()));
}

TEST_F(ConfTest, ClickingACategorySelectsIt) {
  start();
  auto labels = sidebar_labels();
  const size_t example = size_t(std::find(labels.begin(), labels.end(), "Example") - labels.begin());
  auto rows = of_class("sidebar.c", "Selectable");
  ASSERT_EQ(rows.size(), labels.size());
  ASSERT_LT(example, rows.size());
  dynamic payload;
  payload["selected"_key] = true;
  send(rows[example].second, "changed", payload);
  // The panel now shows the Example category's editors.
  EXPECT_FALSE(of_class("panel", "ColorEdit").empty());
  auto selected = of_class("sidebar.c", "Selectable");
  EXPECT_TRUE(selected[example].second->get_as<bool>("selected"_key, false));
  EXPECT_FALSE(selected[0].second->get_as<bool>("selected"_key, true));
}

TEST_F(ConfTest, ModuleSettingsAreListedAndEditable) {
  start();
  select("bdg.desktop.top");
  auto sliders = of_class("panel", "SliderInt");
  ASSERT_EQ(sliders.size(), size_t{2}); // refresh interval, graph history
  dynamic value;
  value["value"_key] = int32_t{300}; // in range for both
  send(sliders[0].second, "changed", value);
  auto entry = srv_->last_session->user_store->get("bdg.desktop.top");
  ASSERT_TRUE(entry.has_value());
  int32_t history = entry->get_as<int32_t>("history"_key, 0);
  int32_t refresh = entry->get_as<int32_t>("refresh_ms"_key, 0);
  EXPECT_EQ(history + refresh, 300); // whichever slider came first
}

// ── Editors ──────────────────────────────────────────────────────────────────

TEST_F(ConfTest, EachTypeGetsItsEditor) {
  start();
  select("example");
  auto switches = of_class("panel", "Checkbox");
  ASSERT_EQ(switches.size(), size_t{1});
  EXPECT_EQ(switches[0].second->get_as<std::string>("style"_key, std::string{}), "switch");
  EXPECT_TRUE(switches[0].second->get_as<bool>("value"_key, false)); // the default
  EXPECT_EQ(of_class("panel", "ColorEdit").size(), size_t{1});
  EXPECT_EQ(of_class("panel", "SliderInt").size(), size_t{1});
  EXPECT_EQ(of_class("panel", "SliderFloat").size(), size_t{1});
  EXPECT_EQ(of_class("panel", "InputText").size(), size_t{1});
  auto combos = of_class("panel", "Combo");
  ASSERT_EQ(combos.size(), size_t{1});
  EXPECT_EQ(combos[0].second->get_as<std::string>("items"_key, std::string{}), "Happy\nCalm");
  // Sections become cards; Banner (2 settings) plus the untitled one.
  int cards = 0;
  for (auto& [path, el] : of_class("panel", "VerticalLayout"))
    cards += el->get_as<bool>("card"_key, false) ? 1 : 0;
  EXPECT_EQ(cards, 3); // header + 2 sections
}

TEST_F(ConfTest, EditingWritesTheUserStore) {
  start();
  select("example");
  dynamic off;
  off["value"_key] = false;
  send(of_class("panel", "Checkbox")[0].second, "changed", off);
  EXPECT_FALSE(std::get<bool>(user_value("enabled", st::setting_type::boolean)));

  dynamic calm;
  calm["value"_key] = int32_t{1};
  send(of_class("panel", "Combo")[0].second, "changed", calm);
  EXPECT_EQ(std::get<std::string>(user_value("mood", st::setting_type::choice)), "calm");

  dynamic green;
  green["value"_key] = std::vector<float>{0.0f, 1.0f, 0.0f};
  send(of_class("panel", "ColorEdit")[0].second, "changed", green);
  EXPECT_EQ(std::get<std::string>(user_value("color", st::setting_type::color)), "#00FF00");

  dynamic text;
  text["value"_key] = std::string{"hi"};
  send(of_class("panel", "InputText")[0].second, "changed", text);
  EXPECT_EQ(std::get<std::string>(user_value("message", st::setting_type::text)), "hi");

  // A customized value shows its reset button; resetting restores the default.
  std::vector<std::pair<std::string, dynamic_ptr>> resets;
  for (auto& [path, el] : of_class("panel", "Button"))
    if (el->get_as<bool>("visible"_key, false))
      resets.emplace_back(path, el);
  ASSERT_FALSE(resets.empty());
  send(resets[0].second, "clicked", dynamic{});
  int customized = 0;
  for (auto& [path, el] : of_class("panel", "Button"))
    customized += el->get_as<bool>("visible"_key, false) ? 1 : 0;
  EXPECT_EQ(customized, int(resets.size()) - 1);
}

TEST_F(ConfTest, InvalidValuesAreRejectedAndShown) {
  start();
  select("example");
  dynamic too_big;
  too_big["value"_key] = int32_t{99};
  send(of_class("panel", "SliderInt")[0].second, "changed", too_big);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(user_value("size", st::setting_type::integer)));
  bool error_shown = false;
  for (auto& [path, el] : of_class("panel", "Label"))
    if (el->get_as<bool>("visible"_key, false) &&
        el->get_as<std::string>("text"_key, std::string{}).find("at most 40") != std::string::npos)
      error_shown = true;
  EXPECT_TRUE(error_shown);
}

// ── Search ───────────────────────────────────────────────────────────────────

TEST_F(ConfTest, SearchListsMatchingSettingsAcrossCategories) {
  start({"alice"});
  auto& objects = srv_->last_session->ui_objects;
  auto it = objects.find(root_ + ".body.sidebar.search");
  ASSERT_NE(it, objects.end());
  dynamic_ptr box{it->second};
  dynamic query;
  query["value"_key] = std::string{"theme"};
  send(box, "changed", query);
  EXPECT_EQ(of_class("panel", "Combo").size(), size_t{1}); // the server theme only
  query["value"_key] = std::string{"banner"};
  send(box, "changed", query);
  EXPECT_EQ(of_class("panel", "InputText").size(), size_t{1}); // "Banner text"
  EXPECT_TRUE(of_class("panel", "Combo").empty());
  query["value"_key] = std::string{"zzz"};
  send(box, "changed", query);
  EXPECT_TRUE(of_class("panel", "Combo").empty());
  EXPECT_TRUE(of_class("panel", "InputText").empty());
}

// ── Server settings ──────────────────────────────────────────────────────────

TEST_F(ConfTest, NonAdminSeesServerSettingsReadOnly) {
  start();
  select("appearance");
  EXPECT_TRUE(of_class("panel", "Combo").empty());
  bool read_only_note = false;
  for (auto& [path, el] : of_class("panel", "Label"))
    if (el->get_as<std::string>("text"_key, std::string{}).find("Only administrators") != std::string::npos)
      read_only_note = true;
  EXPECT_TRUE(read_only_note);
}

TEST_F(ConfTest, AdminEditsServerSettings) {
  start({"alice"});
  select("appearance");
  auto combos = of_class("panel", "Combo");
  ASSERT_EQ(combos.size(), size_t{1});
  dynamic dark;
  dark["value"_key] = int32_t{1};
  send(combos[0].second, "changed", dark);
  auto entry = srv_->last_session->server_store->get("wish.cli.server");
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->get_as<std::string>("theme"_key, std::string{}), "dark");
}

// ── Live refresh ─────────────────────────────────────────────────────────────

TEST_F(ConfTest, RefreshPicksUpNewlyDescribedSettings) {
  start();
  auto first = proxy_->call("refresh"_key, dynamic{}).get();
  EXPECT_FALSE(first.get_as<bool>("changed"_key, true));
  auto tool = make("bdg.dev.tool.verbose", st::setting_type::boolean, "tools");
  st::describe(*srv_->last_session->user_store, tool);
  auto second = proxy_->call("refresh"_key, dynamic{}).get();
  EXPECT_TRUE(second.get_as<bool>("changed"_key, false));
  auto labels = sidebar_labels();
  EXPECT_NE(std::find(labels.begin(), labels.end(), "Tools"), labels.end());
}

TEST_F(ConfTest, AnonymousSessionExplainsWhyItHasNoSettings) {
  start({}, "");
  bool hint = false;
  for (auto& [path, el] : of_class("panel", "Label"))
    if (el->get_as<std::string>("text"_key, std::string{}).find("--username") != std::string::npos)
      hint = true;
  EXPECT_TRUE(hint);
}
