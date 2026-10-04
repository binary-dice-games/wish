// MIT License © 2026 Binary Dice Games
/// @file pkg_backend.hpp
/// @brief What differs between package managers: the commands the pkg module
///        runs for each operation, and how their output is read.
///
/// The pkg module shows one interface over several system package managers.
/// Everything manager-specific is here, as pure functions of a `manager`
/// value: one function per operation that returns the argv to run, and one
/// parser per listing. Adding a manager means adding an enumerator and a
/// case to each function -- nothing else in the module changes.
///
/// No framework / bison / libuv dependency -- unit-tested directly by
/// tests/test_pkg_backend.cpp against captured sample output, since only one
/// of these managers is installed on any given machine.
#pragma once

#include "modules/bdg/common/text.hpp"

#include <optional>
#include <string>
#include <vector>

namespace bdg::wish::pkg {

/// @brief The supported package managers.
enum class manager {
  apt,    ///< Debian / Ubuntu (`apt-get`, `apt-cache`, `dpkg-query`)
  dnf,    ///< Fedora / RHEL (`dnf`, `rpm`)
  pacman, ///< Arch Linux
  brew,   ///< Homebrew (macOS / Linux)
};

/// @brief Every supported manager, in auto-detection order.
const std::vector<manager>& all_managers();

/// @brief The command-line name of @p m (`"apt"`, `"dnf"`, ...).
const char* manager_name(manager m);

/// @brief The manager called @p name, or nullopt for an unknown name.
std::optional<manager> manager_from_name(const std::string& name);

/// @brief `"apt, dnf, pacman, brew"` -- for error messages.
std::string manager_names();

/// @brief How a command that changes the system gets its privileges.
enum class elevation {
  none,   ///< Run as is (already root, or the manager needs none).
  sudo,   ///< `sudo -n`: works with cached credentials or a NOPASSWD rule.
  pkexec, ///< `pkexec`: polkit asks for the password in its own dialog.
};

/// @brief `"none"` / `"sudo"` / `"pkexec"` -> the mode; nullopt for anything
/// else (including `"auto"`, which the caller resolves by probing).
std::optional<elevation> elevation_from_name(const std::string& name);
const char* elevation_name(elevation e);

/// @brief One command to run.
struct command {
  std::vector<std::string> argv; ///< Program + arguments; empty = unsupported.
  bool needs_root{false};        ///< Wrap with elevated() before running.
};

/// @brief One package, as shown in a table. Fields a manager's listing does
/// not provide are left empty.
struct package {
  std::string name;
  std::string version;     ///< Installed version (list) / available version (search).
  std::string latest;      ///< Newer version available (outdated listing only).
  std::string description; ///< One-line summary.
};

// ── Commands ───────────────────────────────────────────────────────────────

/// @brief A cheap command that succeeds only if the manager's CLI is present
/// and runnable (`apt-get --version`, ...). Its first output line is shown as
/// the environment line.
command probe_command(manager m);

/// @brief Lists every installed package; read with parse_installed().
command list_command(manager m);

/// @brief Lists the installed packages a newer version exists for, from the
/// index as last refreshed; read with parse_outdated().
command outdated_command(manager m);

/// @brief Whether @p exit_code from outdated_command() is a success: some
/// managers signal "updates available" (dnf: 100) or "nothing to update"
/// (pacman: 1) through the exit status.
bool outdated_succeeded(manager m, int exit_code);

/// @brief Searches the package index for @p query; read with parse_search().
command search_command(manager m, const std::string& query);

/// @brief Describes one package. More than one command when where to look
/// depends on whether it is installed (pacman): tried in order until one
/// succeeds.
std::vector<command> show_commands(manager m, const std::string& name);

/// @brief Lists the files an installed package owns.
command files_command(manager m, const std::string& name);

command install_command(manager m, const std::vector<std::string>& names);
command remove_command(manager m, const std::string& name);
/// @brief Upgrades one installed package to the newest indexed version.
command upgrade_command(manager m, const std::string& name);
command reinstall_command(manager m, const std::string& name);
/// @brief Upgrades every outdated package.
command upgrade_all_command(manager m);
/// @brief Refreshes the package index from the repositories.
command refresh_index_command(manager m);

/// @brief The argv to actually run for @p cmd: @p cmd.argv, wrapped per
/// @p how when it needs root, with the manager's "never prompt" environment
/// applied (apt: `DEBIAN_FRONTEND=noninteractive`).
std::vector<std::string> elevated(manager m, const command& cmd, elevation how);

// ── Output parsers ─────────────────────────────────────────────────────────

std::vector<package> parse_installed(manager m, const std::string& text);
/// @return Packages with `name` and `latest` set.
std::vector<package> parse_outdated(manager m, const std::string& text);
std::vector<package> parse_search(manager m, const std::string& text);

// ── Validation / messages ──────────────────────────────────────────────────

/// @brief Whether @p name can be passed to a manager as a package name:
/// letters, digits and `@ . _ + : / -`, not starting with `-`.
///
/// Every name the client forwards arrives in an event payload. There is no
/// shell to inject into, but a value such as `--root=/elsewhere` would still
/// be parsed as an option -- possibly by a command running as root.
bool is_valid_package_name(const std::string& name);

/// @brief Whether @p value may be passed as a search query: non-empty and
/// not starting with `-`.
using common::is_safe_arg;

/// @brief Splits an install box's text into package names on whitespace.
std::vector<std::string> split_names(const std::string& text);

/// @brief The package name @p name is known by across listings: without a
/// dpkg architecture qualifier (`libc6:amd64` -> `libc6`), so an outdated
/// entry can be matched to its installed row.
std::string base_name(const std::string& name);

/// @brief Reduces a failed command's stderr to what is worth showing, and
/// explains a refused privilege request (`sudo -n` without cached
/// credentials, a dismissed polkit dialog) in terms of what to do next.
std::string error_summary(elevation how, const std::string& stderr_text);

} // namespace bdg::wish::pkg
