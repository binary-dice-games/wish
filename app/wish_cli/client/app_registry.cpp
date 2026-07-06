// MIT License © 2025 Binary Dice Games
/// @file app_registry.cpp
/// @brief Name -> runner lookup for embedded apps.
#include "app/wish_cli/client/app_registry.hpp"

namespace bdg::wish {

namespace {
std::map<std::string, app_info>& app_map() {
  static std::map<std::string, app_info> apps;
  return apps;
}
} // namespace

void register_app(app_info info) {
  auto name = info.name;
  app_map().emplace(std::move(name), std::move(info));
}

const std::map<std::string, app_info>& registered_apps() {
  return app_map();
}

void describe_app(const app_info& info, std::ostream& out) {
  out << info.name << " - " << info.description << "\n";
  if (info.params.empty()) {
    out << "  (no parameters)\n";
    return;
  }
  out << "Parameters:\n";
  for (const auto& param : info.params)
    out << "  " << param.name << " - " << param.description << "\n";
}

} // namespace bdg::wish
