// MIT License © 2026 Binary Dice Games
/**
 * @file settings_schema.cpp
 * @brief Settings metadata: validation, schema-entry records and JSON.
 */
#include "context/settings_schema.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace bdg::wish::settings {

using namespace bdg::bison;
using json = nlohmann::ordered_json;

namespace {

// ── Helpers ──────────────────────────────────────────────────────────────────

constexpr std::size_t kMaxStoreName = 256;

bool is_field_char(unsigned char c) {
  return std::isalnum(c) || c == '_' || c == '-';
}

bool is_store_name(const std::string& s) {
  if (s.empty() || s.size() > kMaxStoreName)
    return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 0x20 && c <= 0x7E; });
}

bool is_color(const std::string& c) {
  return !c.empty() && c[0] == '#' && (c.size() == 7 || c.size() == 9) &&
      std::all_of(c.begin() + 1, c.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

std::string text_field(const dynamic& d, const char* name) {
  const field* f = d.findField(key_t{name});
  if (!f || !f->is<std::string>())
    return {};
  return f->as<std::string>();
}

/// The index-keyed list stored in field @p name, created if missing.
dynamic& list_field(dynamic& d, const char* name) {
  field& f = d[key_t{name}];
  if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
    static_cast<field_base&>(f) = dynamic_ptr{std::make_shared<dynamic>()};
  return *f.as<dynamic_ptr>();
}

const dynamic* list_field(const dynamic& d, const char* name) {
  const field* f = d.findField(key_t{name});
  if (!f || !f->is<dynamic_ptr>() || !f->as<dynamic_ptr>())
    return nullptr;
  return f->as<dynamic_ptr>().get();
}

/// Replaces the record of @p list whose @p id_field equals @p id with
/// @p record, or appends it.
void upsert(dynamic& list, const char* id_field, const std::string& id, dynamic record) {
  size_t n = list.size();
  for (size_t i = 0; i < n; ++i) {
    field* f = list.findField(key_t{static_cast<uint32_t>(i)});
    if (f && f->is<dynamic_ptr>() && f->as<dynamic_ptr>() && text_field(*f->as<dynamic_ptr>(), id_field) == id) {
      static_cast<field_base&>(*f) = dynamic_ptr{std::make_shared<dynamic>(std::move(record))};
      return;
    }
  }
  static_cast<field_base&>(list[key_t{static_cast<uint32_t>(n)}]) =
      dynamic_ptr{std::make_shared<dynamic>(std::move(record))};
}

template <typename F>
void for_each_record(const dynamic* list, F&& fn) {
  if (!list)
    return;
  size_t n = list->size();
  for (size_t i = 0; i < n; ++i) {
    const field* f = list->findField(key_t{static_cast<uint32_t>(i)});
    if (f && f->is<dynamic_ptr>() && f->as<dynamic_ptr>())
      fn(*f->as<dynamic_ptr>());
  }
}

/// Stores @p value in field @p name (monostate: leaves it unset).
void put_value(dynamic& d, const char* name, const setting_value& value) {
  if (!std::holds_alternative<std::monostate>(value))
    write_value(d, name, value);
}

setting_value get_value(const dynamic& d, const char* name) {
  const field* f = d.findField(key_t{name});
  if (!f)
    return {};
  if (f->is<bool>())
    return f->as<bool>();
  if (f->is<int32_t>())
    return f->as<int32_t>();
  if (f->is<float>())
    return f->as<float>();
  if (f->is<std::string>())
    return f->as<std::string>();
  return {};
}

std::optional<float> get_float(const dynamic& d, const char* name) {
  const field* f = d.findField(key_t{name});
  if (!f || std::holds_alternative<std::monostate>(static_cast<const field_base&>(*f)))
    return std::nullopt;
  try {
    return f->get_as<float>();
  } catch (...) {
    return std::nullopt;
  }
}

/// A value of @p v's JSON type as a setting_value (no coercion).
setting_value from_json_value(const json& v, const std::string& what) {
  if (v.is_null())
    return {};
  if (v.is_boolean())
    return v.get<bool>();
  if (v.is_number_integer()) {
    long long n = v.get<long long>();
    if (n < INT32_MIN || n > INT32_MAX)
      throw schema_error(what + ": out of range");
    return static_cast<int32_t>(n);
  }
  if (v.is_number())
    return v.get<float>();
  if (v.is_string())
    return v.get<std::string>();
  throw schema_error(what + ": expected a bool, number or string");
}

json to_json_value(const setting_value& v) {
  return std::visit(
      [](const auto& x) -> json {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          return nullptr;
        else
          return x;
      },
      v);
}

std::string json_text(const json& obj, const char* key, const std::string& what) {
  auto it = obj.find(key);
  if (it == obj.end() || it->is_null())
    return {};
  if (!it->is_string())
    throw schema_error(what + "." + key + ": expected a string");
  return it->get<std::string>();
}

std::optional<float> json_float(const json& obj, const char* key, const std::string& what) {
  auto it = obj.find(key);
  if (it == obj.end() || it->is_null())
    return std::nullopt;
  if (!it->is_number())
    throw schema_error(what + "." + key + ": expected a number");
  return it->get<float>();
}

json parse_object(const std::string& text, const char* what) {
  json j;
  try {
    j = json::parse(text);
  } catch (const std::exception& ex) {
    throw schema_error(std::string(what) + ": invalid JSON: " + ex.what());
  }
  if (!j.is_object())
    throw schema_error(std::string(what) + ": expected a JSON object");
  return j;
}

} // namespace

// ── Types and names ──────────────────────────────────────────────────────────

const char* type_name(setting_type type) {
  switch (type) {
    case setting_type::boolean:
      return "bool";
    case setting_type::integer:
      return "int";
    case setting_type::real:
      return "float";
    case setting_type::text:
      return "string";
    case setting_type::choice:
      return "enum";
    case setting_type::color:
      return "color";
    case setting_type::path:
      return "path";
  }
  return "string";
}

std::optional<setting_type> parse_type(const std::string& name) {
  for (auto t :
       {setting_type::boolean,
        setting_type::integer,
        setting_type::real,
        setting_type::text,
        setting_type::choice,
        setting_type::color,
        setting_type::path}) {
    if (name == type_name(t))
      return t;
  }
  return std::nullopt;
}

setting_location parse_name(const std::string& name) {
  auto dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 == name.size())
    throw schema_error("setting name '" + name + "': expected <entry>.<field>, e.g. bdg.desktop.example.color");
  setting_location loc{name.substr(0, dot), name.substr(dot + 1)};
  if (!is_store_name(loc.entry) || loc.entry.size() + std::strlen(kSchemaSuffix) > kMaxStoreName ||
      loc.entry.find('#') != std::string::npos)
    throw schema_error("setting name '" + name + "': invalid entry name '" + loc.entry + "'");
  if (!std::all_of(loc.field.begin(), loc.field.end(), [](unsigned char c) { return is_field_char(c); }))
    throw schema_error(
        "setting name '" + name + "': the field '" + loc.field + "' may only use letters, digits, '_' and '-'");
  return loc;
}

std::string category_entry_name(const std::string& id) {
  return schema_entry_name("wish.category." + id);
}

std::string schema_entry_name(const std::string& entry) {
  return entry + kSchemaSuffix;
}

bool is_schema_entry(const std::string& name, std::string* entry) {
  const size_t n = std::strlen(kSchemaSuffix);
  if (name.size() <= n || name.compare(name.size() - n, n, kSchemaSuffix) != 0)
    return false;
  if (entry)
    *entry = name.substr(0, name.size() - n);
  return true;
}

// ── Validation ───────────────────────────────────────────────────────────────

void check(const setting_info& info) {
  parse_name(info.name);
  const std::string what = "setting '" + info.name + "'";
  if (info.min && info.max && *info.min > *info.max)
    throw schema_error(what + ": min is greater than max");
  if (info.step && *info.step <= 0)
    throw schema_error(what + ": step must be positive");
  if (info.type == setting_type::choice) {
    if (info.options.empty())
      throw schema_error(what + ": an enum needs options");
    for (const auto& o : info.options)
      if (o.value.empty())
        throw schema_error(what + ": an option has an empty value");
  }
  if (!std::holds_alternative<std::monostate>(info.default_value)) {
    auto r = validate(info, info.default_value);
    if (auto* err = std::get_if<std::string>(&r))
      throw schema_error(what + ": default: " + *err);
  }
}

void check(const category_info& info) {
  if (info.id.empty() ||
      !std::all_of(info.id.begin(), info.id.end(), [](unsigned char c) { return is_field_char(c) || c == '.'; }))
    throw schema_error("category id '" + info.id + "': use letters, digits, '.', '_' and '-'");
  if (!info.color.empty() && !is_color(info.color))
    throw schema_error("category '" + info.id + "': color must be #RRGGBB or #RRGGBBAA");
}

std::variant<setting_value, std::string> validate(const setting_info& info, const setting_value& value) {
  if (std::holds_alternative<std::monostate>(value))
    return std::string("no value");
  auto in_range = [&](float v) -> std::optional<std::string> {
    if (info.min && v < *info.min)
      return "must be at least " + to_text(*info.min);
    if (info.max && v > *info.max)
      return "must be at most " + to_text(*info.max);
    return std::nullopt;
  };
  switch (info.type) {
    case setting_type::boolean:
      if (auto* b = std::get_if<bool>(&value))
        return setting_value{*b};
      return std::string("expected true or false");
    case setting_type::integer: {
      int32_t v;
      if (auto* i = std::get_if<int32_t>(&value))
        v = *i;
      else if (auto* f = std::get_if<float>(&value); f && std::floor(*f) == *f && std::fabs(*f) < 2.0e9f)
        v = static_cast<int32_t>(*f);
      else
        return std::string("expected a whole number");
      if (auto err = in_range(static_cast<float>(v)))
        return *err;
      return setting_value{v};
    }
    case setting_type::real: {
      float v;
      if (auto* f = std::get_if<float>(&value))
        v = *f;
      else if (auto* i = std::get_if<int32_t>(&value))
        v = static_cast<float>(*i);
      else
        return std::string("expected a number");
      if (!std::isfinite(v))
        return std::string("expected a number");
      if (auto err = in_range(v))
        return *err;
      return setting_value{v};
    }
    case setting_type::text:
    case setting_type::path:
      if (auto* s = std::get_if<std::string>(&value))
        return setting_value{*s};
      return std::string("expected text");
    case setting_type::choice: {
      auto* s = std::get_if<std::string>(&value);
      if (!s)
        return std::string("expected one of the options");
      for (const auto& o : info.options)
        if (o.value == *s)
          return setting_value{*s};
      return "'" + *s + "' is not one of the options";
    }
    case setting_type::color: {
      auto* s = std::get_if<std::string>(&value);
      if (!s || !is_color(*s))
        return std::string("expected a color like #1E3A5F");
      std::string upper = *s;
      std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
      return setting_value{upper};
    }
  }
  return std::string("unknown type");
}

// ── Values ───────────────────────────────────────────────────────────────────

setting_value read_value(const dynamic& entry, const std::string& field_name, setting_type type) {
  const field* f = entry.findField(key_t{field_name.c_str()});
  if (!f || std::holds_alternative<std::monostate>(static_cast<const field_base&>(*f)))
    return {};
  try {
    switch (type) {
      case setting_type::boolean:
        if (f->is<std::string>()) {
          const std::string& s = f->as<std::string>();
          if (s == "true" || s == "1")
            return true;
          if (s == "false" || s == "0")
            return false;
          return {};
        }
        return f->get_as<bool>();
      case setting_type::integer:
        return f->get_as<int32_t>();
      case setting_type::real:
        return f->get_as<float>();
      default:
        return f->get_as<std::string>();
    }
  } catch (...) {
    return {};
  }
}

void write_value(dynamic& entry, const std::string& field_name, const setting_value& value) {
  field& f = entry[key_t{field_name.c_str()}];
  std::visit([&f](const auto& v) { static_cast<field_base&>(f) = v; }, value);
}

std::string to_text(const setting_value& value) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          return {};
        else if constexpr (std::is_same_v<T, bool>)
          return v ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>)
          return v;
        else if constexpr (std::is_same_v<T, float>) {
          std::string s = std::to_string(v);
          s.erase(s.find_last_not_of('0') + 1);
          if (!s.empty() && s.back() == '.')
            s.pop_back();
          return s;
        } else
          return std::to_string(v);
      },
      value);
}

// ── Schema records ───────────────────────────────────────────────────────────

void merge_setting(dynamic& schema, const setting_info& info) {
  dynamic r;
  r["name"_key] = info.name;
  r["label"_key] = info.label;
  r["description"_key] = info.description;
  r["category"_key] = info.category;
  r["section"_key] = info.section;
  r["type"_key] = std::string(type_name(info.type));
  put_value(r, "default", info.default_value);
  if (info.min)
    r["min"_key] = *info.min;
  if (info.max)
    r["max"_key] = *info.max;
  if (info.step)
    r["step"_key] = *info.step;
  if (!info.options.empty()) {
    auto list = std::make_shared<dynamic>();
    for (size_t i = 0; i < info.options.size(); ++i) {
      auto o = std::make_shared<dynamic>();
      (*o)["value"_key] = info.options[i].value;
      (*o)["label"_key] = info.options[i].label;
      (*list)[key_t{static_cast<uint32_t>(i)}] = dynamic_ptr{o};
    }
    r["options"_key] = dynamic_ptr{list};
  }
  r["icon"_key] = info.icon;
  r["order"_key] = info.order;
  r["restart"_key] = info.restart;
  upsert(list_field(schema, "settings"), "name", info.name, std::move(r));
}

void merge_category(dynamic& schema, const category_info& info) {
  dynamic r;
  r["id"_key] = info.id;
  r["label"_key] = info.label;
  r["description"_key] = info.description;
  r["icon"_key] = info.icon;
  r["color"_key] = info.color;
  r["order"_key] = info.order;
  upsert(list_field(schema, "categories"), "id", info.id, std::move(r));
}

std::vector<setting_info> settings_of(const dynamic& schema) {
  std::vector<setting_info> out;
  for_each_record(list_field(schema, "settings"), [&](const dynamic& r) {
    setting_info s;
    s.name = text_field(r, "name");
    auto type = parse_type(text_field(r, "type"));
    if (!type)
      return;
    s.type = *type;
    s.label = text_field(r, "label");
    s.description = text_field(r, "description");
    s.category = text_field(r, "category");
    s.section = text_field(r, "section");
    s.default_value = get_value(r, "default");
    s.min = get_float(r, "min");
    s.max = get_float(r, "max");
    s.step = get_float(r, "step");
    for_each_record(list_field(r, "options"), [&](const dynamic& o) {
      s.options.push_back({text_field(o, "value"), text_field(o, "label")});
    });
    s.icon = text_field(r, "icon");
    s.order = r.get_as<int32_t>("order"_key, 0);
    s.restart = r.get_as<bool>("restart"_key, false);
    try {
      check(s);
    } catch (const schema_error&) {
      return;
    }
    out.push_back(std::move(s));
  });
  return out;
}

std::vector<category_info> categories_of(const dynamic& schema) {
  std::vector<category_info> out;
  for_each_record(list_field(schema, "categories"), [&](const dynamic& r) {
    category_info c;
    c.id = text_field(r, "id");
    c.label = text_field(r, "label");
    c.description = text_field(r, "description");
    c.icon = text_field(r, "icon");
    c.color = text_field(r, "color");
    c.order = r.get_as<int32_t>("order"_key, 0);
    try {
      check(c);
    } catch (const schema_error&) {
      return;
    }
    out.push_back(std::move(c));
  });
  return out;
}

// ── JSON ─────────────────────────────────────────────────────────────────────

std::string to_json(const setting_info& info) {
  json j;
  j["name"] = info.name;
  j["label"] = info.label;
  j["description"] = info.description;
  j["category"] = info.category;
  j["section"] = info.section;
  j["type"] = type_name(info.type);
  j["default"] = to_json_value(info.default_value);
  if (info.min)
    j["min"] = *info.min;
  if (info.max)
    j["max"] = *info.max;
  if (info.step)
    j["step"] = *info.step;
  if (!info.options.empty()) {
    j["options"] = json::array();
    for (const auto& o : info.options)
      j["options"].push_back({{"value", o.value}, {"label", o.label}});
  }
  j["icon"] = info.icon;
  j["order"] = info.order;
  j["restart"] = info.restart;
  return j.dump();
}

std::string to_json(const category_info& info) {
  json j;
  j["id"] = info.id;
  j["label"] = info.label;
  j["description"] = info.description;
  j["icon"] = info.icon;
  j["color"] = info.color;
  j["order"] = info.order;
  return j.dump();
}

setting_info setting_from_json(const std::string& text) {
  json j = parse_object(text, "setting");
  setting_info s;
  s.name = json_text(j, "name", "setting");
  const std::string what = "setting '" + s.name + "'";
  s.label = json_text(j, "label", what);
  s.description = json_text(j, "description", what);
  s.category = json_text(j, "category", what);
  s.section = json_text(j, "section", what);
  std::string type = json_text(j, "type", what);
  if (type.empty())
    type = "string";
  auto t = parse_type(type);
  if (!t)
    throw schema_error(what + ": unknown type '" + type + "' (bool, int, float, string, enum, color or path)");
  s.type = *t;
  if (auto it = j.find("default"); it != j.end())
    s.default_value = from_json_value(*it, what + ".default");
  s.min = json_float(j, "min", what);
  s.max = json_float(j, "max", what);
  s.step = json_float(j, "step", what);
  if (auto it = j.find("options"); it != j.end() && !it->is_null()) {
    if (!it->is_array())
      throw schema_error(what + ".options: expected a list");
    for (const auto& o : *it) {
      if (o.is_string())
        s.options.push_back({o.get<std::string>(), {}});
      else if (o.is_object())
        s.options.push_back({json_text(o, "value", what + ".options"), json_text(o, "label", what + ".options")});
      else
        throw schema_error(what + ".options: expected strings or {value, label} objects");
    }
  }
  s.icon = json_text(j, "icon", what);
  if (auto it = j.find("order"); it != j.end() && !it->is_null()) {
    if (!it->is_number_integer())
      throw schema_error(what + ".order: expected a whole number");
    s.order = it->get<int32_t>();
  }
  if (auto it = j.find("restart"); it != j.end() && !it->is_null()) {
    if (!it->is_boolean())
      throw schema_error(what + ".restart: expected true or false");
    s.restart = it->get<bool>();
  }
  // A float default for an int setting written as 3.0, etc.
  if (!std::holds_alternative<std::monostate>(s.default_value)) {
    auto r = validate(s, s.default_value);
    if (auto* v = std::get_if<setting_value>(&r))
      s.default_value = *v;
  }
  check(s);
  return s;
}

category_info category_from_json(const std::string& text) {
  json j = parse_object(text, "category");
  category_info c;
  c.id = json_text(j, "id", "category");
  const std::string what = "category '" + c.id + "'";
  c.label = json_text(j, "label", what);
  c.description = json_text(j, "description", what);
  c.icon = json_text(j, "icon", what);
  c.color = json_text(j, "color", what);
  if (auto it = j.find("order"); it != j.end() && !it->is_null()) {
    if (!it->is_number_integer())
      throw schema_error(what + ".order: expected a whole number");
    c.order = it->get<int32_t>();
  }
  check(c);
  return c;
}

} // namespace bdg::wish::settings
