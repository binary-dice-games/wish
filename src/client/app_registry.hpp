// MIT License © 2025 Binary Dice Games
/**
 * @file app_registry.hpp
 * @brief Name -> runner lookup for embedded apps, shared by `wish client`,
 *        `wish standalone`, and `wish_client_dll` (and therefore Python).
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

  /// @brief Where this app's module lives in the modules/<organization>/
  ///        <collection>/<name> tree (see modules/README.md) -- e.g. "bdg"
  ///        and "desktop" for the bundled calculator/notepad/process_explorer.
  ///        Populated automatically by wish_add_module() via the
  ///        WISH_MODULE_ORGANIZATION/WISH_MODULE_COLLECTION compile
  ///        definitions it attaches per-module (see
  ///        modules/bdg/desktop/calculator/client/calculator.cpp for the
  ///        pattern); empty for a module registered without that convention
  ///        (e.g. a bare `wish_add_module(<name>)` with no org/collection
  ///        segments).
  std::string organization;
  std::string collection;

  std::string description;
  std::vector<app_param> params;
  AppFn run;
};

/// @brief Registers an embedded app. Each optional module's client runner
///        calls this from a static-initialized registrar object (see
///        modules/bdg/desktop/calculator/client/calculator.cpp for the
///        pattern) --
///        callers outside a module's own translation unit should not call
///        this directly.
///
/// Two different modules may legitimately register the same short `.name`
/// (e.g. `bdg/desktop/calculator` and `microsoft/tools/calculator`, both
/// named `"calculator"`) -- this always succeeds and both remain visible
/// (see `registered_apps()`/`resolve_app()`), rather than one silently
/// shadowing the other.
void register_app(app_info info);

/// @brief Short name -> registration info table, populated by
///        register_app(). A `std::multimap` (not `std::map`): the same
///        short name may have more than one entry if two different
///        modules' apps happen to share it -- see `resolve_app()` for how
///        callers disambiguate that case. Iterate this directly for
///        `--list`/`wish_list_apps()`, which show every registered app
///        regardless of collisions.
const std::multimap<std::string, app_info>& registered_apps();

/// @brief Outcome of resolve_app().
enum class app_resolve_status {
  found,      ///< Exactly one match; `info` is valid.
  ambiguous,  ///< More than one app shares this short name; `candidates` lists their qualified names.
  not_found,  ///< No app matches `name` at all (as either a short or qualified name).
};

/// @brief Result of resolve_app().
struct app_resolution {
  app_resolve_status status = app_resolve_status::not_found;
  const app_info* info = nullptr;       ///< Valid iff status == found.
  std::vector<std::string> candidates;  ///< Qualified names of every match; populated iff status == ambiguous.
};

/// @brief Resolves `name` -- either a short leaf name ("calculator") or a
///        fully-qualified "<organization>/<collection>/<name>"
///        (see `qualified_app_name()`) -- to exactly one registered app,
///        for `--describe=<name>`/`--run=<name>`/`wish_run_app()`.
///
/// A fully-qualified name always resolves unambiguously (assuming no two
/// modules register under the exact same organization/collection/name,
/// which would itself be a build-time collision the module authors are
/// responsible for avoiding). A short name resolves unambiguously only when
/// exactly one registered app carries it; if more than one does (see
/// `register_app()`), the caller must use the fully-qualified form instead --
/// `format_ambiguous_error()` renders a message telling them so.
app_resolution resolve_app(const std::string& name);

/// @brief Formats an "ambiguous app name" error, e.g. "'calculator' is
///        ambiguous between: bdg/desktop/calculator,
///        microsoft/tools/calculator -- use the fully-qualified name."
///        `candidates` is `app_resolution::candidates` from a
///        `resolve_app()` call whose `status` was `ambiguous`.
std::string format_ambiguous_error(const std::string& name, const std::vector<std::string>& candidates);

/// @brief Prints `info`'s name, description, and parameters to `out`, for
///        `--describe=<name>`.
void describe_app(const app_info& info, std::ostream& out);

/// @brief "<organization>/<collection>/<name>" (see modules/README.md), or
///        just `info.name` if organization/collection weren't populated --
///        used by `--list`/`wish_list_apps()` and ambiguity errors so it's
///        clear which module owns each app.
std::string qualified_app_name(const app_info& info);

} // namespace bdg::wish
