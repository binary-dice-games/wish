// MIT License © 2026 Binary Dice Games
/**
 * @file settings_store.cpp
 * @brief Described settings in a persistent_store.
 */
#include "context/settings_store.hpp"

#include "src/bison/bison_sync.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace bdg::wish::settings {

using namespace bdg::bison;

void describe(persistent_store& store, const setting_info& info) {
  check(info);
  const std::string name = schema_entry_name(parse_name(info.name).entry);
  dynamic schema = store.get(name).value_or(dynamic{});
  merge_setting(schema, info);
  store.set(name, schema);
}

void describe_category(persistent_store& store, const category_info& info) {
  check(info);
  const std::string name = category_entry_name(info.id);
  dynamic schema = store.get(name).value_or(dynamic{});
  merge_category(schema, info);
  store.set(name, schema);
}

catalog collect(const persistent_store& store, bool with_registered) {
  catalog out;
  std::set<std::string> category_ids;
  std::set<std::string> setting_names;
  if (with_registered) {
    // Registered metadata comes from the running server's code, so it wins
    // over a description stored by some earlier version.
    for (auto& c : registered_categories())
      if (category_ids.insert(c.id).second)
        out.categories.push_back(std::move(c));
    for (auto& s : registered_settings()) {
      if (!setting_names.insert(s.name).second)
        continue;
      const setting_location loc = parse_name(s.name);
      described_setting d;
      if (auto entry = store.get(loc.entry))
        d.value = read_value(*entry, loc.field, s.type);
      d.info = std::move(s);
      out.settings.push_back(std::move(d));
    }
  }
  for (const std::string& key : store.keys()) {
    std::string entry_name;
    if (!is_schema_entry(key, &entry_name))
      continue;
    auto schema = store.get(key);
    if (!schema)
      continue;
    for (auto& c : categories_of(*schema))
      if (category_ids.insert(c.id).second)
        out.categories.push_back(std::move(c));
    auto entry = store.get(entry_name);
    for (auto& s : settings_of(*schema)) {
      // A record must describe a field of the entry it is filed under.
      if (parse_name(s.name).entry != entry_name || !setting_names.insert(s.name).second)
        continue;
      described_setting d;
      d.value = entry ? read_value(*entry, parse_name(s.name).field, s.type) : setting_value{};
      d.info = std::move(s);
      out.settings.push_back(std::move(d));
    }
  }
  std::stable_sort(out.categories.begin(), out.categories.end(), [](const category_info& a, const category_info& b) {
    return std::tie(a.order, a.label, a.id) < std::tie(b.order, b.label, b.id);
  });
  std::stable_sort(
      out.settings.begin(), out.settings.end(), [](const described_setting& a, const described_setting& b) {
        return std::tie(a.info.category, a.info.section, a.info.order, a.info.label, a.info.name) <
            std::tie(b.info.category, b.info.section, b.info.order, b.info.label, b.info.name);
      });
  return out;
}

std::string write_setting(persistent_store& store, const setting_info& info, const setting_value& value) {
  auto checked = validate(info, value);
  if (auto* err = std::get_if<std::string>(&checked))
    return *err;
  const setting_location loc = parse_name(info.name);
  dynamic entry = store.get(loc.entry).value_or(dynamic{});
  write_value(entry, loc.field, std::get<setting_value>(checked));
  try {
    store.set(loc.entry, entry);
  } catch (const std::exception& ex) {
    return ex.what();
  }
  return {};
}

setting_value value_of(const persistent_store* store, const setting_info& info) {
  if (store) {
    const setting_location loc = parse_name(info.name);
    if (auto entry = store->get(loc.entry)) {
      setting_value v = read_value(*entry, loc.field, info.type);
      if (!std::holds_alternative<std::monostate>(v)) {
        auto checked = validate(info, v);
        if (auto* ok = std::get_if<setting_value>(&checked))
          return *ok;
      }
    }
  }
  return info.default_value;
}

bool bool_of(const persistent_store* store, const setting_info& info) {
  auto v = value_of(store, info);
  auto* b = std::get_if<bool>(&v);
  return b ? *b : false;
}

int32_t int_of(const persistent_store* store, const setting_info& info) {
  auto v = value_of(store, info);
  if (auto* i = std::get_if<int32_t>(&v))
    return *i;
  if (auto* f = std::get_if<float>(&v))
    return static_cast<int32_t>(*f);
  return 0;
}

float float_of(const persistent_store* store, const setting_info& info) {
  auto v = value_of(store, info);
  if (auto* f = std::get_if<float>(&v))
    return *f;
  if (auto* i = std::get_if<int32_t>(&v))
    return static_cast<float>(*i);
  return 0.0f;
}

std::string text_of(const persistent_store* store, const setting_info& info) {
  auto v = value_of(store, info);
  auto* s = std::get_if<std::string>(&v);
  return s ? *s : std::string{};
}

// ── Registry ─────────────────────────────────────────────────────────────────

namespace {

struct registry_state {
  std::vector<setting_info> settings;
  std::vector<category_info> categories;
};

bison::synchronized<registry_state>& registry() {
  static bison::synchronized<registry_state> r;
  return r;
}

} // namespace

void register_setting(const setting_info& info) {
  check(info);
  auto r = registry().wlock();
  auto it =
      std::find_if(r->settings.begin(), r->settings.end(), [&](const setting_info& s) { return s.name == info.name; });
  if (it != r->settings.end())
    *it = info;
  else
    r->settings.push_back(info);
}

void register_category(const category_info& info) {
  check(info);
  auto r = registry().wlock();
  auto it =
      std::find_if(r->categories.begin(), r->categories.end(), [&](const category_info& c) { return c.id == info.id; });
  if (it != r->categories.end())
    *it = info;
  else
    r->categories.push_back(info);
}

std::vector<setting_info> registered_settings() {
  return registry().rlock()->settings;
}

std::vector<category_info> registered_categories() {
  return registry().rlock()->categories;
}

void reset_setting(persistent_store& store, const setting_info& info) {
  const setting_location loc = parse_name(info.name);
  auto entry = store.get(loc.entry);
  if (!entry || !entry->findField(key_t{loc.field.c_str()}))
    return;
  write_value(*entry, loc.field, setting_value{});
  store.set(loc.entry, *entry);
}

} // namespace bdg::wish::settings
