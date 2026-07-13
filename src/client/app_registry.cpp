// MIT License © 2025 Binary Dice Games
/// @file app_registry.cpp
/// @brief Name -> runner lookup for embedded apps.
#include <client/app_registry.hpp>

#include <sstream>

namespace bdg::wish {

namespace {
std::multimap<std::string, app_info>& app_map() {
  static std::multimap<std::string, app_info> apps;
  return apps;
}
} // namespace

void register_app(app_info info) {
  auto name = info.name;
  app_map().insert({std::move(name), std::move(info)});
}

const std::multimap<std::string, app_info>& registered_apps() {
  return app_map();
}

std::string qualified_app_name(const app_info& info) {
  if (info.organization.empty() && info.collection.empty())
    return info.name;
  return info.organization + "/" + info.collection + "/" + info.name;
}

app_resolution resolve_app(const std::string& name) {
  // A fully-qualified name always wins outright, even if it also happens to
  // equal some other app's short name (unlikely, but qualified beats short).
  for (const auto& [short_name, info] : app_map()) {
    if (qualified_app_name(info) == name)
      return {app_resolve_status::found, &info, {}};
  }

  auto range = app_map().equal_range(name);
  if (range.first == range.second)
    return {app_resolve_status::not_found, nullptr, {}};

  auto next = range.first;
  ++next;
  if (next == range.second)
    return {app_resolve_status::found, &range.first->second, {}};

  app_resolution result{app_resolve_status::ambiguous, nullptr, {}};
  for (auto it = range.first; it != range.second; ++it)
    result.candidates.push_back(qualified_app_name(it->second));
  return result;
}

std::string format_ambiguous_error(const std::string& name, const std::vector<std::string>& candidates) {
  std::ostringstream oss;
  oss << "'" << name << "' is ambiguous between:";
  for (const auto& candidate : candidates)
    oss << " " << candidate << ",";
  std::string msg = oss.str();
  msg.pop_back(); // trailing comma
  msg += " -- use the fully-qualified name.";
  return msg;
}

void describe_app(const app_info& info, std::ostream& out) {
  out << qualified_app_name(info) << " - " << info.description << "\n";
  if (info.params.empty()) {
    out << "  (no parameters)\n";
    return;
  }
  out << "Parameters:\n";
  for (const auto& param : info.params)
    out << "  " << param.name << " - " << param.description << "\n";
}

} // namespace bdg::wish
