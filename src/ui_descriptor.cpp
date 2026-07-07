// MIT License © 2025 Binary Dice Games
/// @file ui_descriptor.cpp
/// @brief Parses JSON/YAML text into a generic bison::dynamic tree.
///
/// JSON is parsed with nlohmann/json (nlohmann::ordered_json, preserving
/// object key declaration order so children get an accurate default "order"
/// stamp). YAML is parsed with libyaml event-by-event into an equivalent
/// ordered_json representation and fed through the same node-building path.
///
/// No `bison::dynamic` class registry lookups happen here — the resulting
/// tree is plain, untyped `dynamic` objects, so this file has no dependency
/// on ui_element.hpp/registry.hpp and can be linked into a client-only
/// binary (see CMakeLists.txt: added to both wish_client and wish_server).
#include <ui_descriptor.hpp>

#include <nlohmann/json.hpp>
#include <yaml.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace bdg::wish {

using namespace bdg::bison;
// ordered_json preserves JSON object key insertion order so that named
// children are processed — and stamped with 'order' — in declaration order.
using json = nlohmann::ordered_json;

// ── Field copy (no prototype to coerce against — that happens server-side) ──

static void set_field_from_json(dynamic& obj, key_t field_key, const json& value) {
  if (value.is_boolean())
    obj[field_key] = value.get<bool>();
  else if (value.is_number_integer())
    obj[field_key] = value.get<int32_t>();
  else if (value.is_number_float())
    obj[field_key] = value.get<float>();
  else if (value.is_string())
    obj[field_key] = value.get<std::string>();
  // Other JSON value kinds (null, nested object/array as a scalar field) are
  // silently ignored — only "type" and "children" nodes are ever objects.
}

// ── JSON → generic dynamic tree ───────────────────────────────────────────────

static dynamic import_json_node(const json& descriptor);

static dynamic_ptr build_children(const json& children_json) {
  auto children_dyn = dynamic_ptr{key_t{0U}, {}};
  int32_t order_counter = 0;

  auto process_named = [&](const std::string& name, const json& child_json) {
    dynamic child = import_json_node(child_json);
    child["__name__"_key] = name;
    if (!child_json.contains("order"))
      child["order"_key] = order_counter;
    ++order_counter;
    (*children_dyn)[key_t{name}] = dynamic_ptr{std::move(child)};
  };

  auto process_indexed = [&](size_t idx, const json& child_json) {
    dynamic child = import_json_node(child_json);
    if (!child_json.contains("order"))
      child["order"_key] = order_counter;
    ++order_counter;
    (*children_dyn)[idx] = dynamic_ptr{std::move(child)};
  };

  if (children_json.is_array()) {
    for (size_t i = 0; i < children_json.size(); ++i) {
      process_indexed(i, children_json[i]);
    }
  } else if (children_json.is_object()) {
    for (const auto& [name, child_json] : children_json.items()) {
      bool numeric =
          !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); });
      if (numeric) {
        process_indexed(std::stoul(name), child_json);
      } else {
        process_named(name, child_json);
      }
    }
  }

  return children_dyn;
}

static dynamic import_json_node(const json& descriptor) {
  if (!descriptor.is_object()) {
    throw std::runtime_error("wish::import_descriptor_json: descriptor node must be a JSON object");
  }

  auto type_it = descriptor.find("type");
  if (type_it == descriptor.end() || !type_it->is_string()) {
    throw std::runtime_error("wish::import_descriptor_json: node missing required \"type\" field");
  }

  dynamic obj;
  obj["__type__"_key] = key_t{type_it->get_ref<const std::string&>()};

  for (const auto& [key_str, value] : descriptor.items()) {
    if (key_str == "type" || key_str == "children")
      continue;
    // Skip reserved bookkeeping fields that should never come from a
    // descriptor authored by hand (they're stamped by this importer).
    if (key_str.size() >= 2 && key_str[0] == '_' && key_str[1] == '_')
      continue;
    set_field_from_json(obj, key_t{key_str}, value);
  }

  auto children_it = descriptor.find("children");
  if (children_it != descriptor.end()) {
    obj["children"_key] = build_children(*children_it);
  }

  return obj;
}

dynamic import_descriptor_json(const std::string& json_text) {
  json parsed;
  try {
    parsed = json::parse(json_text);
  } catch (const json::exception& e) {
    throw std::runtime_error(std::string{"wish::import_descriptor_json: "} + e.what());
  }
  return import_json_node(parsed);
}

// ── YAML → JSON ───────────────────────────────────────────────────────────────

// RAII wrapper so yaml_event_t is always deleted, even on throw.
struct YamlEvent {
  yaml_event_t ev{};
  explicit YamlEvent(yaml_parser_t* p) {
    if (!yaml_parser_parse(p, &ev)) {
      throw std::runtime_error("wish::import_descriptor_yaml: YAML parse error");
    }
  }
  ~YamlEvent() {
    yaml_event_delete(&ev);
  }
  yaml_event_type_t type() const {
    return ev.type;
  }
};

static json yaml_scalar_to_json(const yaml_event_t& ev) {
  const char* raw = reinterpret_cast<const char*>(ev.data.scalar.value);
  bool plain = ev.data.scalar.style == YAML_PLAIN_SCALAR_STYLE;

  // Use json(val) not json{val}: brace-init hits the initializer_list ctor
  // and wraps the value in an array.
  if (!plain)
    return json(std::string(raw));

  std::string s(raw);
  if (s == "true" || s == "yes" || s == "on")
    return json(true);
  if (s == "false" || s == "no" || s == "off")
    return json(false);
  if (s == "null" || s == "~")
    return json(nullptr);

  // Try integer first, then float.
  try {
    size_t pos{};
    long long iv = std::stoll(s, &pos);
    if (pos == s.size())
      return json(static_cast<int32_t>(iv));
  } catch (...) {
  }

  try {
    size_t pos{};
    double dv = std::stod(s, &pos);
    if (pos == s.size())
      return json(static_cast<float>(dv));
  } catch (...) {
  }

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
    if (key_ev.type() == YAML_MAPPING_END_EVENT)
      break;
    if (key_ev.type() != YAML_SCALAR_EVENT) {
      throw std::runtime_error("wish::import_descriptor_yaml: expected scalar mapping key");
    }
    std::string key{reinterpret_cast<const char*>(key_ev.ev.data.scalar.value)};
    obj[key] = parse_yaml_value(p);
  }
  return obj;
}

static json parse_yaml_sequence(yaml_parser_t* p) {
  json arr = json::array();
  while (true) {
    // Peek at the next event type; if sequence end, stop.
    YamlEvent ev{p};
    if (ev.type() == YAML_SEQUENCE_END_EVENT)
      break;

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

dynamic import_descriptor_yaml(const std::string& yaml_text) {
  // Normalise CRLF → LF so that raw string literals compiled on Windows
  // (which have \r\n endings) don't produce \r-suffixed scalar values.
  std::string normalized;
  normalized.reserve(yaml_text.size());
  for (char c : yaml_text) {
    if (c != '\r')
      normalized += c;
  }

  yaml_parser_t parser{};
  if (!yaml_parser_initialize(&parser)) {
    throw std::runtime_error("wish::import_descriptor_yaml: failed to initialize YAML parser");
  }

  yaml_parser_set_input_string(&parser, reinterpret_cast<const unsigned char*>(normalized.data()), normalized.size());

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
          throw std::runtime_error("wish::import_descriptor_yaml: empty or unreadable YAML document");
        default:
          break; // skip STREAM_START, DOCUMENT_START, etc.
      }
    }
  } catch (...) {
    yaml_parser_delete(&parser);
    throw;
  }

  yaml_parser_delete(&parser);

  return import_json_node(parsed);
}

// ── Text sniffing ─────────────────────────────────────────────────────────────

dynamic import_descriptor_text(const std::string& text) {
  auto it = std::find_if_not(text.cbegin(), text.cend(), [](unsigned char c) { return std::isspace(c); });
  bool is_json = (it != text.cend() && (*it == '{' || *it == '['));
  return is_json ? import_descriptor_json(text) : import_descriptor_yaml(text);
}

} // namespace bdg::wish
