// MIT License © 2025 Binary Dice Games
/// @file app_registry.cpp
/// @brief Name -> runner lookup for embedded apps.
#include "app/wish_cli/client/apps/app_registry.hpp"

namespace bdg::wish {

namespace {
std::map<std::string, AppFn>& app_map() {
  static std::map<std::string, AppFn> apps;
  return apps;
}
} // namespace

void register_app(const std::string& name, AppFn fn) {
  app_map().emplace(name, std::move(fn));
}

const std::map<std::string, AppFn>& registered_apps() {
  return app_map();
}

} // namespace bdg::wish
