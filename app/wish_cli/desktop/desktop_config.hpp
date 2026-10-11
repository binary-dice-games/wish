// MIT License © 2026 Binary Dice Games
/**
 * @file desktop_config.hpp
 * @brief The `wish desktop` configuration file (`~/.wish/desktop/desktop.yaml`)
 *        and its `start_menu` section.
 *
 * The file is a YAML (or JSON) map of sections, each configuring one part of
 * the desktop. Only `start_menu` is defined so far; unknown sections are
 * ignored, so a later feature adds a section without changing the format.
 * See docs/desktop-config.md.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bdg::wish::desktop {

/// @brief A config file error; the message names the offending path, e.g.
///        `start_menu.items[2].run: expected a list or a string`.
class config_error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// @brief What a Start menu entry is.
enum class entry_kind {
  run, ///< Starts a process (`run:`).
  action, ///< Calls a desktop action (`action:`).
  folder, ///< Holds other entries (`items:`).
  separator, ///< A separator line (`separator: true`).
};

/// @brief One Start menu entry, after placeholder expansion.
struct menu_entry {
  entry_kind kind{entry_kind::run};
  std::string id; ///< Unique; explicit `id`, or derived from the labels.
  std::string label;
  std::string icon; ///< e.g. "res/icons/edit.png"; empty for none.
  std::string tooltip;
  /// The `"app"` a client started by this entry connects as: ties its
  /// windows to the entry (taskbar icon, running mark). From `app:`, or
  /// the `--run=<name>` argument of the command.
  std::string app;
  std::vector<std::string> argv; ///< run
  std::vector<std::pair<std::string, std::string>> env; ///< run
  std::string cwd; ///< run
  std::string action; ///< action
  nlohmann::ordered_json args = nlohmann::ordered_json::object(); ///< action
  bool pinned{false}; ///< Pinned to the taskbar initially.
  bool on_desktop{false}; ///< A desktop icon initially.
  std::vector<menu_entry> children; ///< folder
};

/// @brief The `start_menu` section.
struct start_menu {
  std::vector<menu_entry> items; ///< The Start menu tree.
  std::vector<menu_entry> power; ///< The power button's entries.

  /// @brief Every entry, depth-first (folders included), for lookups.
  std::vector<const menu_entry*> all() const;
  /// @brief The entry with @p id, or nullptr.
  const menu_entry* find(const std::string& id) const;
};

/// @brief Values substituted into `run`, `cwd` and `env`.
struct placeholders {
  /// `{wish_client}`: the tokens that run a wish client, e.g.
  /// `{"/usr/bin/wish", "client"}`; expands to all of them as a whole
  /// `run` token.
  std::vector<std::string> wish_client;
  std::string config_file; ///< `{config_file}`
  std::string home; ///< `{home}`
};

/// @brief Names `action:` entries may use (the desktop's action table).
const std::vector<std::string>& known_actions();

/**
 * @brief Parse a `start_menu` section.
 *
 * Expands `apps:` entries from the app registry (`registered_apps()`) and
 * the placeholders, derives missing ids, and applies the section's
 * `pinned:` / `desktop:` id lists.
 *
 * @throws config_error naming the offending path.
 */
start_menu parse_start_menu(const nlohmann::ordered_json& section, const placeholders& ph);

/// @brief The configuration written when the file doesn't exist.
const std::string& default_config_text();

/// @brief Result of loading the config file.
struct loaded_config {
  /// The Start menu: the file's, or the default one when the file has no
  /// `start_menu` section or (with @ref error set) can't be used.
  start_menu menu;
  /// Why the file couldn't be used; empty when it was fine.
  std::string error;
  /// True when the file didn't exist and the default was written to it.
  bool wrote_default{false};
};

/**
 * @brief Read @p file, writing the default configuration there first if it
 *        doesn't exist. Never throws: a file that can't be read or parsed
 *        yields the default menu and an @ref loaded_config::error.
 */
loaded_config load_config(const std::filesystem::path& file, const placeholders& ph);

/**
 * @brief Parse configuration @p text (YAML or JSON).
 * @return The Start menu (the default one when there is no `start_menu`
 *         section).
 * @throws config_error / std::runtime_error on a syntax or schema error.
 */
start_menu parse_config_text(const std::string& text, const placeholders& ph);

/// @brief Split a command line on spaces, honoring '…' and "…" quotes.
std::vector<std::string> split_command(const std::string& command);

} // namespace bdg::wish::desktop
