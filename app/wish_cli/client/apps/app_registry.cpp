// MIT License © 2025 Binary Dice Games
/// @file app_registry.cpp
/// @brief Name -> runner lookup for embedded apps.
#include "app/wish_cli/client/apps/app_registry.hpp"

#include "app/wish_cli/client/apps/calculator.hpp"
#include "app/wish_cli/client/apps/notepad.hpp"
#include "app/wish_cli/client/apps/process_explorer/process_explorer.hpp"

namespace bdg::wish {

const std::map<std::string, AppFn>& registered_apps() {
  static const std::map<std::string, AppFn> apps = {
      {"calculator", run_calculator},
      {"notepad", run_notepad},
      {"process_explorer", run_process_explorer},
  };
  return apps;
}

} // namespace bdg::wish
