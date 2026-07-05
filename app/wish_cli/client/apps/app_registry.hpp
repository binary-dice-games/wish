// MIT License © 2025 Binary Dice Games
/**
 * @file app_registry.hpp
 * @brief Name -> runner lookup for embedded apps, shared by `wish client`
 *        and `wish standalone`.
 */
#pragma once

#include <functional>
#include <map>
#include <string>

namespace bdg::wish {

class wish_app_host;

/// @brief An embedded app's entry point: sets up proxies/event handlers, then
///        returns (the caller blocks on session completion separately).
using AppFn = std::function<void(wish_app_host&)>;

/// @brief Name -> runner table for `calculator`, `notepad`, `process_explorer`.
const std::map<std::string, AppFn>& registered_apps();

} // namespace bdg::wish
