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

/// @brief Registers an embedded app under `name`. Each optional module's
///        client runner calls this from a static-initialized registrar
///        object (see modules/calculator/client/calculator.cpp for the
///        pattern) -- callers outside a module's own translation unit should
///        not call this directly.
void register_app(const std::string& name, AppFn fn);

/// @brief Name -> runner table, populated by register_app().
const std::map<std::string, AppFn>& registered_apps();

} // namespace bdg::wish
