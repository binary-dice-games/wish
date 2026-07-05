// MIT License © 2025 Binary Dice Games
/**
 * @file app_registry.hpp
 * @brief Name -> runner lookup for embedded apps, shared by `wish client`
 *        and `wish standalone`.
 */
#pragma once

#include <functional>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace bdg::wish {

class wish_app_host;

/// @brief An embedded app's entry point: sets up proxies/event handlers, then
///        returns (the caller blocks on session completion separately).
using AppFn = std::function<void(wish_app_host&)>;

/// @brief Describes one positional parameter an app reads via
///        `wish_app_host::app_args()` (the tokens following a literal `--`
///        on the command line).
struct app_param {
  std::string name;
  std::string description;
};

/// @brief Full registration info for an embedded app, as reported by
///        `--list` and `--describe=<name>`.
struct app_info {
  std::string name;
  std::string description;
  std::vector<app_param> params;
  AppFn run;
};

/// @brief Registers an embedded app. Each optional module's client runner
///        calls this from a static-initialized registrar object (see
///        modules/calculator/client/calculator.cpp for the pattern) --
///        callers outside a module's own translation unit should not call
///        this directly.
void register_app(app_info info);

/// @brief Name -> registration info table, populated by register_app().
const std::map<std::string, app_info>& registered_apps();

/// @brief Prints `info`'s name, description, and parameters to `out`, for
///        `--describe=<name>`.
void describe_app(const app_info& info, std::ostream& out);

} // namespace bdg::wish
