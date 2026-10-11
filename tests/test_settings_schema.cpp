// MIT License © 2026 Binary Dice Games
// Tests for src/context/settings_schema.hpp and settings_store.hpp -- the
// settings metadata model behind the conf tool.
#include "context/settings_schema.hpp"
#include "context/settings_store.hpp"
#include "client/wish_app_host.hpp"
#include "src/rmi/shared/ids.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>

using namespace bdg::bison;
namespace st = bdg::wish::settings;

namespace {

st::setting_info color_setting() {
  st::setting_info s;
  s.name = "bdg.desktop.example.color";
  s.label = "Color";
  s.category = "example";
  s.type = st::setting_type::color;
  s.default_value = std::string{"#FF0000"};
  return s;
}

std::filesystem::path temp_store_file() {
  return std::filesystem::temp_directory_path() /
      ("wish_settings_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_" +
       std::to_string(bdg::bison::rmi::shared::generate_id().id) + ".bison");
}

std::string error_of(const std::variant<st::setting_value, std::string>& r) {
  auto* e = std::get_if<std::string>(&r);
  return e ? *e : std::string{};
}

} // namespace

// ── Names ────────────────────────────────────────────────────────────────────

TEST(SettingsSchema, ParseNameSplitsAtTheLastDot) {
  auto loc = st::parse_name("bdg.desktop.example.color");
  EXPECT_EQ(loc.entry, "bdg.desktop.example");
  EXPECT_EQ(loc.field, "color");
  EXPECT_EQ(st::schema_entry_name(loc.entry), "bdg.desktop.example#schema");
  std::string entry;
  EXPECT_TRUE(st::is_schema_entry("bdg.desktop.example#schema", &entry));
  EXPECT_EQ(entry, "bdg.desktop.example");
  EXPECT_FALSE(st::is_schema_entry("bdg.desktop.example"));
}

TEST(SettingsSchema, ParseNameRejectsBadNames) {
  for (const char* bad : {"color", ".color", "a.", "a#b.c", "a.b c", "a.b/c"})
    EXPECT_THROW(st::parse_name(bad), st::schema_error) << bad;
  EXPECT_THROW(st::parse_name(std::string(260, 'a') + ".f"), st::schema_error);
}

// ── Validation ───────────────────────────────────────────────────────────────

TEST(SettingsSchema, ValidateChecksAndCoercesEachType) {
  st::setting_info s = color_setting();
  auto ok = st::validate(s, std::string{"#1e3a5f"});
  ASSERT_TRUE(std::holds_alternative<st::setting_value>(ok));
  EXPECT_EQ(std::get<std::string>(std::get<st::setting_value>(ok)), "#1E3A5F");
  EXPECT_NE(error_of(st::validate(s, std::string{"blue"})), "");

  s.type = st::setting_type::integer;
  s.min = 1.0f;
  s.max = 10.0f;
  EXPECT_EQ(error_of(st::validate(s, int32_t{5})), "");
  EXPECT_EQ(std::get<int32_t>(std::get<st::setting_value>(st::validate(s, 3.0f))), 3); // coerced
  EXPECT_NE(error_of(st::validate(s, 2.5f)), "");
  EXPECT_NE(error_of(st::validate(s, int32_t{11})).find("at most 10"), std::string::npos);
  EXPECT_NE(error_of(st::validate(s, int32_t{0})).find("at least 1"), std::string::npos);

  s.type = st::setting_type::real;
  EXPECT_FLOAT_EQ(std::get<float>(std::get<st::setting_value>(st::validate(s, int32_t{2}))), 2.0f);

  s.type = st::setting_type::choice;
  s.options = {{"dark", "Dark"}, {"light", "Light"}};
  EXPECT_EQ(error_of(st::validate(s, std::string{"dark"})), "");
  EXPECT_NE(error_of(st::validate(s, std::string{"blue"})), "");

  s.type = st::setting_type::boolean;
  EXPECT_EQ(error_of(st::validate(s, true)), "");
  EXPECT_NE(error_of(st::validate(s, std::string{"yes"})), "");
  EXPECT_NE(error_of(st::validate(s, st::setting_value{})), "");
}

TEST(SettingsSchema, CheckRejectsInconsistentMetadata) {
  st::setting_info s = color_setting();
  EXPECT_NO_THROW(st::check(s));
  s.default_value = int32_t{3};
  EXPECT_THROW(st::check(s), st::schema_error); // default doesn't match the type
  s = color_setting();
  s.type = st::setting_type::choice;
  s.default_value = {};
  EXPECT_THROW(st::check(s), st::schema_error); // enum without options
  s = color_setting();
  s.type = st::setting_type::integer;
  s.default_value = {};
  s.min = 5.0f;
  s.max = 1.0f;
  EXPECT_THROW(st::check(s), st::schema_error);
  st::category_info c{"example", "Example", "", "", "blue", 0};
  EXPECT_THROW(st::check(c), st::schema_error); // color must be #RRGGBB
  c.color = "#2F7BF5";
  EXPECT_NO_THROW(st::check(c));
}

// ── Schema records ───────────────────────────────────────────────────────────

TEST(SettingsSchema, MergeAddsAndReplacesRecords) {
  dynamic schema;
  st::merge_setting(schema, color_setting());
  st::setting_info count;
  count.name = "bdg.desktop.example.count";
  count.type = st::setting_type::integer;
  count.min = 0.0f;
  count.max = 99.0f;
  count.default_value = int32_t{3};
  st::merge_setting(schema, count);
  auto changed = color_setting();
  changed.label = "Accent color";
  st::merge_setting(schema, changed); // same name: replaced in place

  auto all = st::settings_of(schema);
  ASSERT_EQ(all.size(), size_t{2});
  EXPECT_EQ(all[0].label, "Accent color");
  EXPECT_EQ(std::get<std::string>(all[0].default_value), "#FF0000");
  EXPECT_EQ(all[1].name, "bdg.desktop.example.count");
  EXPECT_EQ(std::get<int32_t>(all[1].default_value), 3);
  EXPECT_FLOAT_EQ(*all[1].max, 99.0f);

  st::merge_category(schema, {"example", "Example", "Demo settings", "res/icons/palette.png", "#AF52DE", 5});
  st::merge_category(schema, {"example", "Example!", "", "", "", 1});
  auto cats = st::categories_of(schema);
  ASSERT_EQ(cats.size(), size_t{1});
  EXPECT_EQ(cats[0].label, "Example!");
}

TEST(SettingsSchema, JsonRoundTrip) {
  st::setting_info s;
  s.name = "bdg.desktop.example.mode";
  s.label = "Mode";
  s.description = "How it works";
  s.category = "example";
  s.section = "Behavior";
  s.type = st::setting_type::choice;
  s.options = {{"a", "Alpha"}, {"b", "Beta"}};
  s.default_value = std::string{"b"};
  s.icon = "res/icons/tune.png";
  s.order = 2;
  s.restart = true;
  st::setting_info back = st::setting_from_json(st::to_json(s));
  EXPECT_EQ(back.name, s.name);
  EXPECT_EQ(back.section, "Behavior");
  ASSERT_EQ(back.options.size(), size_t{2});
  EXPECT_EQ(back.options[1].label, "Beta");
  EXPECT_EQ(std::get<std::string>(back.default_value), "b");
  EXPECT_TRUE(back.restart);
  EXPECT_EQ(back.order, 2);

  // Short forms: options as plain strings, a float default for an int.
  auto j = st::setting_from_json(R"({"name": "a.b.size", "type": "int", "default": 4.0, "min": 1, "max": 9})");
  EXPECT_EQ(std::get<int32_t>(j.default_value), 4);
  auto e = st::setting_from_json(R"({"name": "a.b.m", "type": "enum", "options": ["x", "y"]})");
  EXPECT_EQ(e.options[0].value, "x");

  auto c = st::category_from_json(R"({"id": "example", "label": "Example", "color": "#AF52DE", "order": 3})");
  EXPECT_EQ(c.order, 3);
  EXPECT_EQ(st::category_from_json(st::to_json(c)).color, "#AF52DE");
}

TEST(SettingsSchema, JsonErrorsAreSchemaErrors) {
  EXPECT_THROW(st::setting_from_json("{"), st::schema_error);
  EXPECT_THROW(st::setting_from_json("[1]"), st::schema_error);
  EXPECT_THROW(st::setting_from_json(R"({"name": "a.b.c", "type": "rainbow"})"), st::schema_error);
  EXPECT_THROW(st::setting_from_json(R"({"name": "nodot"})"), st::schema_error);
  EXPECT_THROW(st::setting_from_json(R"({"name": "a.b.c", "type": "bool", "default": "x"})"), st::schema_error);
  EXPECT_THROW(st::category_from_json(R"({"id": ""})"), st::schema_error);
}

TEST(SettingsSchema, ReadAndWriteValues) {
  dynamic entry;
  st::write_value(entry, "count", int32_t{7});
  EXPECT_EQ(std::get<int32_t>(st::read_value(entry, "count", st::setting_type::integer)), 7);
  // A value of another type converts (CLI defaults store some numbers as text).
  st::write_value(entry, "size", std::string{"12"});
  EXPECT_EQ(std::get<int32_t>(st::read_value(entry, "size", st::setting_type::integer)), 12);
  st::write_value(entry, "size", int32_t{13}); // the type may change
  EXPECT_EQ(std::get<int32_t>(st::read_value(entry, "size", st::setting_type::integer)), 13);
  st::write_value(entry, "on", std::string{"true"});
  EXPECT_TRUE(std::get<bool>(st::read_value(entry, "on", st::setting_type::boolean)));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(st::read_value(entry, "missing", st::setting_type::text)));
  st::write_value(entry, "count", st::setting_value{});
  EXPECT_TRUE(std::holds_alternative<std::monostate>(st::read_value(entry, "count", st::setting_type::integer)));
  EXPECT_EQ(st::to_text(1.5f), "1.5");
  EXPECT_EQ(st::to_text(true), "true");
}

// ── Stores ───────────────────────────────────────────────────────────────────

TEST(SettingsStore, DescribeCollectWriteAndReset) {
  auto file = temp_store_file();
  {
    bdg::wish::persistent_store store(file);
    st::describe(store, color_setting());
    st::setting_info on;
    on.name = "bdg.desktop.example.enabled";
    on.label = "Enabled";
    on.category = "example";
    on.type = st::setting_type::boolean;
    on.default_value = true;
    st::describe(store, on);
    st::describe_category(store, {"example", "Example", "", "", "#AF52DE", 0});
    // Another app's entry, in another category.
    st::setting_info theme;
    theme.name = "wish.desktop.theme";
    theme.category = "desktop";
    theme.type = st::setting_type::choice;
    theme.options = {{"light", ""}, {"dark", ""}};
    st::describe(store, theme);

    auto c = st::collect(store);
    ASSERT_EQ(c.categories.size(), size_t{1});
    ASSERT_EQ(c.settings.size(), size_t{3});
    EXPECT_EQ(c.settings[0].info.category, "desktop"); // sorted by category
    EXPECT_EQ(c.settings[1].info.label, "Color");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(c.settings[1].value));
    EXPECT_EQ(std::get<std::string>(c.settings[1].effective()), "#FF0000");

    EXPECT_EQ(st::write_setting(store, color_setting(), std::string{"#00ff00"}), "");
    EXPECT_NE(st::write_setting(store, color_setting(), std::string{"green"}), "");
    // Other fields of the entry are kept.
    auto entry = store.get("bdg.desktop.example").value_or(dynamic{});
    entry["other"_key] = int32_t{1};
    store.set("bdg.desktop.example", entry);
    EXPECT_EQ(st::write_setting(store, on, false), "");
    entry = *store.get("bdg.desktop.example");
    EXPECT_EQ(entry.get_as<int32_t>("other"_key, 0), 1);

    c = st::collect(store);
    EXPECT_EQ(std::get<std::string>(c.settings[1].value), "#00FF00");
    EXPECT_FALSE(std::get<bool>(c.settings[2].value));

    st::reset_setting(store, color_setting());
    c = st::collect(store);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(c.settings[1].value));
  }
  // Everything survives a reload from disk.
  bdg::wish::persistent_store reloaded(file);
  auto c = st::collect(reloaded);
  ASSERT_EQ(c.settings.size(), size_t{3});
  EXPECT_FALSE(std::get<bool>(c.settings[2].value));
  EXPECT_EQ(c.categories[0].color, "#AF52DE");
  std::filesystem::remove(file);
}

TEST(SettingsStore, RecordsFiledUnderAnotherEntryAreIgnored) {
  auto file = temp_store_file();
  bdg::wish::persistent_store store(file);
  dynamic schema;
  st::merge_setting(schema, color_setting()); // describes bdg.desktop.example.color ...
  store.set("other.app#schema", schema); // ... but filed under other.app
  EXPECT_TRUE(st::collect(store).settings.empty());
  std::filesystem::remove(file);
}

// ── Values and the registry ──────────────────────────────────────────────────

TEST(SettingsStore, ValueOfFallsBackToTheDefault) {
  auto file = temp_store_file();
  bdg::wish::persistent_store store(file);
  st::setting_info count;
  count.name = "bdg.desktop.example.count";
  count.type = st::setting_type::integer;
  count.min = 1;
  count.max = 10;
  count.default_value = int32_t{5};
  EXPECT_EQ(st::int_of(nullptr, count), 5); // anonymous session
  EXPECT_EQ(st::int_of(&store, count), 5); // unset
  dynamic entry;
  entry["count"_key] = int32_t{7};
  store.set("bdg.desktop.example", entry);
  EXPECT_EQ(st::int_of(&store, count), 7);
  EXPECT_FLOAT_EQ(st::float_of(&store, count), 7.0f);
  entry["count"_key] = int32_t{70}; // out of range
  store.set("bdg.desktop.example", entry);
  EXPECT_EQ(st::int_of(&store, count), 5);
  EXPECT_EQ(st::text_of(&store, color_setting()), "#FF0000");
  std::filesystem::remove(file);
}

TEST(SettingsStore, RegisteredSettingsAreCollectedWithTheirValues) {
  st::setting_info lines;
  lines.name = "test.registry.app.lines";
  lines.category = "test.registry.app";
  lines.type = st::setting_type::integer;
  lines.default_value = int32_t{100};
  st::register_setting(lines);
  lines.label = "Lines";
  st::register_setting(lines); // replaces
  st::register_category({"test.registry.app", "App", "", "", "#FF9500", 0});
  st::setting_info bad;
  bad.name = "no-dot";
  EXPECT_THROW(st::register_setting(bad), st::schema_error);

  auto registered = st::registered_settings();
  auto n = std::count_if(registered.begin(), registered.end(), [](const st::setting_info& s) {
    return s.name == "test.registry.app.lines";
  });
  EXPECT_EQ(n, 1);

  auto file = temp_store_file();
  bdg::wish::persistent_store store(file);
  dynamic entry;
  entry["lines"_key] = int32_t{42};
  store.set("test.registry.app", entry);
  // A stale description in the store loses to the registered one.
  st::setting_info stale = lines;
  stale.label = "Old";
  st::describe(store, stale);

  auto c = st::collect(store, true);
  auto it = std::find_if(c.settings.begin(), c.settings.end(), [](const st::described_setting& d) {
    return d.info.name == "test.registry.app.lines";
  });
  ASSERT_NE(it, c.settings.end());
  EXPECT_EQ(it->info.label, "Lines");
  EXPECT_EQ(std::get<int32_t>(it->value), 42);
  EXPECT_EQ(std::count_if(c.settings.begin(), c.settings.end(), [](const st::described_setting& d) {
              return d.info.name == "test.registry.app.lines";
            }),
            1);
  EXPECT_TRUE(std::any_of(c.categories.begin(), c.categories.end(), [](const st::category_info& ci) {
    return ci.id == "test.registry.app" && ci.color == "#FF9500";
  }));
  std::filesystem::remove(file);
}

namespace {

/// wish_app_host over an in-memory user store.
class store_host : public bdg::wish::wish_app_host {
 public:
  bool anonymous = false;
  std::map<std::string, dynamic> entries;

  std::future<rmi::proxy::dynamic> instantiate(bdg::bison::key_t, bdg::bison::key_t, dynamic) override {
    throw std::logic_error("unused");
  }
  std::future<void> upload_file(const std::string&, const std::string&, bdg::wish::transfer_progress_callback)
      override {
    throw std::logic_error("unused");
  }
  std::future<std::string> download_file(const std::string&, bdg::wish::transfer_progress_callback) override {
    throw std::logic_error("unused");
  }
  bool has_user_store() const override {
    return !anonymous;
  }
  std::future<std::optional<dynamic>> user_store_get(const std::string& name) override {
    std::promise<std::optional<dynamic>> p;
    auto it = entries.find(name);
    p.set_value(it == entries.end() ? std::nullopt : std::optional<dynamic>{it->second});
    return p.get_future();
  }
  std::future<void> user_store_set(const std::string& name, dynamic value) override {
    entries[name] = std::move(value);
    std::promise<void> p;
    p.set_value();
    return p.get_future();
  }
  void keep_alive(rmi::proxy::dynamic&&) override {}
  void signal_done() override {}
  const std::vector<std::string>& app_args() const override {
    return args_;
  }
  bool read_console_line(std::string&) override {
    return false;
  }

 private:
  std::vector<std::string> args_;
};

} // namespace

TEST(SettingsAppHost, ReadAndWriteSettings) {
  store_host host;
  st::setting_info on;
  on.name = "bdg.desktop.example.enabled";
  on.type = st::setting_type::boolean;
  on.default_value = true;
  EXPECT_TRUE(host.read_bool_setting(on));
  EXPECT_EQ(host.write_setting(on, false), "");
  EXPECT_FALSE(host.read_bool_setting(on));
  EXPECT_EQ(host.write_setting(color_setting(), std::string{"#00ff00"}), "");
  EXPECT_NE(host.write_setting(color_setting(), std::string{"green"}), "");
  EXPECT_EQ(host.read_text_setting(color_setting()), "#00FF00");
  // Both fields share the entry.
  EXPECT_FALSE(host.entries["bdg.desktop.example"].get_as<bool>("enabled"_key, true));

  host.anonymous = true;
  EXPECT_TRUE(host.read_bool_setting(on)); // the default
  EXPECT_NE(host.write_setting(on, false), "");
}
