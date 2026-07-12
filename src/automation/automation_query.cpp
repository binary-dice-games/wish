// MIT License © 2025 Binary Dice Games
/// @file automation_query.cpp
/// @brief Implementation of bdg::wish::automation.
#include <automation/automation_query.hpp>

#ifdef WISH_AUTOMATION_ENABLED

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_print.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

namespace bdg::wish::automation {

using namespace bdg::bison;

namespace {

// "Well-known" content fields probed on every element, generically -- no
// per-class schema is maintained (see build_tree_snapshot()'s doc comment).
// A widget class that has none of these (e.g. Separator) simply contributes
// none of these keys to its JSON entry.
struct probe_field {
  const char* name;
  key_t key;
};

constexpr probe_field kProbedFields[] = {
    {"label", "label"_key},
    {"text", "text"_key},
    {"value", "value"_key},
    {"title", "title"_key},
    {"checked", "checked"_key},
    {"selected", "selected"_key},
    {"hint", "hint"_key},
};

// True if `path` is exactly `root` or a dot-descendant of it. Empty `root`
// matches every path (the "whole tree" query).
bool under_root(const std::string& path, const std::string& root) {
  if (root.empty() || path == root)
    return true;
  return path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '.';
}

std::string hex_hash(hash_t h) {
  std::ostringstream os;
  os << "0x" << std::hex << std::setfill('0') << std::setw(8) << h;
  return os.str();
}

// Copies whichever of kProbedFields are present on `elem` (as their actual
// stored type) into `out`.
void add_probed_fields(const ui_element& elem, nlohmann::json& out) {
  for (const auto& pf : kProbedFields) {
    if (const auto* s = elem.findField<std::string>(pf.key))
      out[pf.name] = *s;
    else if (const auto* b = elem.findField<bool>(pf.key))
      out[pf.name] = *b;
    else if (const auto* i = elem.findField<int32_t>(pf.key))
      out[pf.name] = *i;
    else if (const auto* f = elem.findField<float>(pf.key))
      out[pf.name] = *f;
  }
}

void add_hit_test(const ui_element& elem, const hit_test_map& hits, nlohmann::json& out) {
  key_t id = elem.get_as<key_t>("__wish_id"_key, key_t{});
  auto it = id.id != 0 ? hits.find(id) : hits.end();
  if (it == hits.end()) {
    // Never rendered this frame (e.g. inside a collapsed window, an
    // unopened tab, or a hidden subtree) -- report "no known position"
    // rather than guessing.
    out["rect"] = nullptr;
    out["hovered"] = false;
    out["active"] = false;
    out["visible"] = false;
    return;
  }
  const hit_test_entry& h = it->second;
  out["rect"] = {{"x0", h.x0}, {"y0", h.y0}, {"x1", h.x1}, {"y1", h.y1}};
  out["hovered"] = h.hovered;
  out["active"] = h.active;
  out["visible"] = h.visible;
}

} // namespace

std::optional<query_tree_request> parse_query_tree_request(const std::string& json_payload) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(json_payload);
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
  if (!j.is_object() || !j.contains("request_id") || !j["request_id"].is_number_unsigned())
    return std::nullopt;

  query_tree_request req;
  req.request_id = j["request_id"].get<uint32_t>();
  if (j.contains("root") && j["root"].is_string())
    req.root = j["root"].get<std::string>();
  return req;
}

std::string build_tree_snapshot(
    uint32_t request_id, const std::string& root, const wish::ui_tree& ui_objects, const hit_test_map& hits) {
  // Sorted so a snapshot's widget order is deterministic (repeatable test
  // assertions, stable diffs) rather than following unordered_map's
  // unspecified iteration order.
  std::vector<std::pair<std::string, ui_element_ptr>> sorted(ui_objects.begin(), ui_objects.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  auto class_names = build_display_dict();

  nlohmann::json widgets = nlohmann::json::array();
  for (const auto& [path, elem] : sorted) {
    if (!elem || !under_root(path, root))
      continue;

    nlohmann::json w;
    w["path"] = path;

    key_t klass = elem->get_as<key_t>(dynamic::CLASS, key_t{});
    auto class_it = class_names.find(klass.id);
    w["class"] = class_it != class_names.end() ? class_it->second : hex_hash(klass.id);

    add_probed_fields(*elem, w);
    add_hit_test(*elem, hits, w);

    widgets.push_back(std::move(w));
  }

  nlohmann::json out;
  out["request_id"] = request_id;
  out["widgets"] = std::move(widgets);
  return out.dump();
}

std::string build_log_event(const std::deque<logger::log_entry>& new_entries) {
  nlohmann::json entries = nlohmann::json::array();
  for (const auto& e : new_entries) {
    nlohmann::json entry;
    entry["seq"] = e.seq;
    entry["timestamp"] = e.timestamp;
    entry["level"] = e.level;
    entry["message"] = e.message;
    entries.push_back(std::move(entry));
  }

  nlohmann::json out;
  out["logs"] = std::move(entries);
  return out.dump();
}

} // namespace bdg::wish::automation

#endif // WISH_AUTOMATION_ENABLED
