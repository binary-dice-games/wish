// MIT License © 2025 Binary Dice Games
/// @file ui_importer.cpp
/// @brief Parses JSON and YAML descriptors into live wish object trees.
///
/// JSON is parsed with nlohmann/json (preserving string keys for name_map
/// path building).  YAML is parsed with libyaml into an equivalent JSON
/// representation and then fed through the same JSON processing path.
#include <wish/ui_importer.hpp>

#include "src/bison/bison_object.hpp"

#include <nlohmann/json.hpp>
#include <yaml.h>

#include <cctype>
#include <stdexcept>
#include <string>

namespace bdg::wish {

using namespace bdg::bison;
// ordered_json preserves JSON object key insertion order so that named
// children are processed — and stamped with 'order' — in declaration sequence.
using json = nlohmann::ordered_json;

// ── Field coercion ────────────────────────────────────────────────────────────

/// Set one field on @p obj from a JSON value, coercing numeric types as
/// needed to match the prototype field type (e.g. JSON integer → float field).
static void set_field_from_json(
    dynamic& obj, key_t field_key, const json& value) {
  auto& dst = obj[field_key];

  if (dst.is<std::monostate>()) {
    // Field not in prototype: accept whatever JSON gives us.
    if (value.is_boolean())             dst = value.get<bool>();
    else if (value.is_number_integer()) dst = value.get<int32_t>();
    else if (value.is_number_float())   dst = value.get<float>();
    else if (value.is_string())         dst = value.get<std::string>();
  } else if (dst.is<bool>() && value.is_boolean()) {
    dst = value.get<bool>();
  } else if (dst.is<int32_t>()) {
    if (value.is_number_integer())      dst = value.get<int32_t>();
    else if (value.is_number_float())   dst = static_cast<int32_t>(value.get<float>());
  } else if (dst.is<float>()) {
    if (value.is_number_float())        dst = value.get<float>();
    else if (value.is_number_integer()) dst = static_cast<float>(value.get<int32_t>());
  } else if (dst.is<std::string>() && value.is_string()) {
    dst = value.get<std::string>();
  }
  // Type mismatches for unknown fields are silently ignored.
}

// ── JSON → wish objects ───────────────────────────────────────────────────────

static ui_element_ptr import_json_node(
    const json& descriptor,
    const std::string& path,
    bool add_to_map,
    name_map& result);

static dynamic_ptr build_children(
    const json& children_json,
    const std::string& parent_path,
    bool parent_in_map,
    name_map& result) {

  auto children_dyn = dynamic_ptr{key_t{0U}, {}};
  int32_t order_counter = 0;

  auto process_named = [&](const std::string& name, const json& child_json) {
    std::string child_path;
    bool child_in_map = parent_in_map;
    if (child_in_map) {
      child_path = parent_path.empty() ? name : (parent_path + "." + name);
    }
    auto child = import_json_node(child_json, child_path, child_in_map, result);
    if (!child_json.contains("order"))
      (*child)["order"_key] = order_counter;
    ++order_counter;
    (*children_dyn)[key_t{name}] = child;
  };

  auto process_indexed = [&](size_t idx, const json& child_json) {
    auto child = import_json_node(child_json, "", false, result);
    if (!child_json.contains("order"))
      (*child)["order"_key] = order_counter;
    ++order_counter;
    (*children_dyn)[idx] = child;
  };

  if (children_json.is_array()) {
    for (size_t i = 0; i < children_json.size(); ++i) {
      process_indexed(i, children_json[i]);
    }
  } else if (children_json.is_object()) {
    for (const auto& [name, child_json] : children_json.items()) {
      bool numeric = !name.empty() &&
          std::all_of(name.begin(), name.end(),
              [](unsigned char c) { return std::isdigit(c); });
      if (numeric) {
        process_indexed(std::stoul(name), child_json);
      } else {
        process_named(name, child_json);
      }
    }
  }

  return children_dyn;
}

static ui_element_ptr import_json_node(
    const json& descriptor,
    const std::string& path,
    bool add_to_map,
    name_map& result) {

  if (!descriptor.is_object()) {
    throw std::runtime_error(
        "wish::import_json: descriptor node must be a JSON object");
  }

  auto type_it = descriptor.find("type");
  if (type_it == descriptor.end() || !type_it->is_string()) {
    throw std::runtime_error(
        "wish::import_json: node missing required \"type\" field");
  }

  const std::string& type_str = type_it->get_ref<const std::string&>();
  key_t type_key{type_str};

  {
    auto lp = dynamic::getRegistry().rlock();
    auto ns_it = lp->find("wish"_key);
    if (ns_it == lp->end() ||
        ns_it->second.find(type_key) == ns_it->second.end()) {
      throw std::runtime_error(
          "wish::import_json: unknown element type \"" + type_str + "\"");
    }
  }

  auto obj = dynamic::instantiate<ui_element>("wish"_key, type_key);

  for (const auto& [key_str, value] : descriptor.items()) {
    if (key_str == "type" || key_str == "children") continue;
    // Skip reserved bison fields that should never come from descriptors.
    if (key_str.size() >= 2 && key_str[0] == '_' && key_str[1] == '_') continue;
    set_field_from_json(*obj, key_t{key_str}, value);
  }

  auto children_it = descriptor.find("children");
  if (children_it != descriptor.end()) {
    (*obj)["children"_key] = build_children(
        *children_it, path, add_to_map, result);
    obj->refresh_children_order();
  }

  if (add_to_map) {
    result[path] = obj;
  }

  return obj;
}

// ── Public JSON entry point ───────────────────────────────────────────────────

name_map import_json(const std::string& json_str) {
  json parsed;
  try {
    parsed = json::parse(json_str);
  } catch (const json::exception& e) {
    throw std::runtime_error(std::string{"wish::import_json: "} + e.what());
  }

  name_map result;
  import_json_node(parsed, "", true, result);
  return result;
}

// ── YAML → JSON ───────────────────────────────────────────────────────────────

// RAII wrapper so yaml_event_t is always deleted, even on throw.
struct YamlEvent {
  yaml_event_t ev{};
  explicit YamlEvent(yaml_parser_t* p) {
    if (!yaml_parser_parse(p, &ev)) {
      throw std::runtime_error("wish::import_yaml: YAML parse error");
    }
  }
  ~YamlEvent() { yaml_event_delete(&ev); }
  yaml_event_type_t type() const { return ev.type; }
};

static json yaml_scalar_to_json(const yaml_event_t& ev) {
  const char* raw =
      reinterpret_cast<const char*>(ev.data.scalar.value);
  bool plain = ev.data.scalar.style == YAML_PLAIN_SCALAR_STYLE;

  // Use json(val) not json{val}: brace-init hits the initializer_list ctor
  // and wraps the value in an array.
  if (!plain) return json(std::string(raw));

  std::string s(raw);
  if (s == "true"  || s == "yes" || s == "on")  return json(true);
  if (s == "false" || s == "no"  || s == "off") return json(false);
  if (s == "null"  || s == "~")                 return json(nullptr);

  // Try integer first, then float.
  try {
    size_t pos{};
    long long iv = std::stoll(s, &pos);
    if (pos == s.size())
      return json(static_cast<int32_t>(iv));
  } catch (...) {}

  try {
    size_t pos{};
    double dv = std::stod(s, &pos);
    if (pos == s.size())
      return json(static_cast<float>(dv));
  } catch (...) {}

  return json(s);
}

// Forward declarations for mutual recursion.
static json parse_yaml_mapping(yaml_parser_t* p);
static json parse_yaml_sequence(yaml_parser_t* p);

static json parse_yaml_value(yaml_parser_t* p) {
  YamlEvent ev{p};
  switch (ev.type()) {
    case YAML_SCALAR_EVENT:
      return yaml_scalar_to_json(ev.ev);
    case YAML_MAPPING_START_EVENT:
      return parse_yaml_mapping(p);
    case YAML_SEQUENCE_START_EVENT:
      return parse_yaml_sequence(p);
    default:
      return json{};
  }
}

static json parse_yaml_mapping(yaml_parser_t* p) {
  json obj = json::object();
  while (true) {
    YamlEvent key_ev{p};
    if (key_ev.type() == YAML_MAPPING_END_EVENT) break;
    if (key_ev.type() != YAML_SCALAR_EVENT) {
      throw std::runtime_error(
          "wish::import_yaml: expected scalar mapping key");
    }
    std::string key{
        reinterpret_cast<const char*>(key_ev.ev.data.scalar.value)};
    obj[key] = parse_yaml_value(p);
  }
  return obj;
}

static json parse_yaml_sequence(yaml_parser_t* p) {
  json arr = json::array();
  while (true) {
    // Peek at the next event type; if sequence end, stop.
    YamlEvent ev{p};
    if (ev.type() == YAML_SEQUENCE_END_EVENT) break;

    switch (ev.type()) {
      case YAML_SCALAR_EVENT:
        arr.push_back(yaml_scalar_to_json(ev.ev));
        break;
      case YAML_MAPPING_START_EVENT:
        arr.push_back(parse_yaml_mapping(p));
        break;
      case YAML_SEQUENCE_START_EVENT:
        arr.push_back(parse_yaml_sequence(p));
        break;
      default:
        break;
    }
  }
  return arr;
}

// ── Public YAML entry point ───────────────────────────────────────────────────

name_map import_yaml(const std::string& yaml_str) {
  // Normalise CRLF → LF so that raw string literals compiled on Windows
  // (which have \r\n endings) don't produce \r-suffixed scalar values.
  std::string normalized;
  normalized.reserve(yaml_str.size());
  for (char c : yaml_str) {
    if (c != '\r') normalized += c;
  }

  yaml_parser_t parser{};
  if (!yaml_parser_initialize(&parser)) {
    throw std::runtime_error(
        "wish::import_yaml: failed to initialize YAML parser");
  }

  yaml_parser_set_input_string(
      &parser,
      reinterpret_cast<const unsigned char*>(normalized.data()),
      normalized.size());

  json parsed;
  try {
    // Skip preamble events (STREAM_START, DOCUMENT_START, etc.) until the
    // first content node is reached.
    bool found = false;
    while (!found) {
      YamlEvent ev{&parser};
      switch (ev.type()) {
        case YAML_MAPPING_START_EVENT:
          parsed = parse_yaml_mapping(&parser);
          found = true;
          break;
        case YAML_SEQUENCE_START_EVENT:
          parsed = parse_yaml_sequence(&parser);
          found = true;
          break;
        case YAML_SCALAR_EVENT:
          parsed = yaml_scalar_to_json(ev.ev);
          found = true;
          break;
        case YAML_STREAM_END_EVENT:
        case YAML_NO_EVENT:
          throw std::runtime_error(
              "wish::import_yaml: empty or unreadable YAML document");
        default:
          break;  // skip STREAM_START, DOCUMENT_START, etc.
      }
    }
  } catch (...) {
    yaml_parser_delete(&parser);
    throw;
  }

  yaml_parser_delete(&parser);

  name_map result;
  import_json_node(parsed, "", true, result);
  return result;
}

}  // namespace bdg::wish
