// MIT License © 2026 Binary Dice Games
/**
 * @file settings_store.cpp
 * @brief Described settings in a persistent_store.
 */
#include "context/settings_store.hpp"

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

catalog collect(const persistent_store& store) {
  catalog out;
  std::set<std::string> category_ids;
  std::set<std::string> setting_names;
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

void reset_setting(persistent_store& store, const setting_info& info) {
  const setting_location loc = parse_name(info.name);
  auto entry = store.get(loc.entry);
  if (!entry || !entry->findField(key_t{loc.field.c_str()}))
    return;
  write_value(*entry, loc.field, setting_value{});
  store.set(loc.entry, *entry);
}

} // namespace bdg::wish::settings
