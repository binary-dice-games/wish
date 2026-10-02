// MIT License © 2026 Binary Dice Games
/// @file pkg_source.hpp
/// @brief Client-side package-manager orchestration for the pkg module.
///
/// Owns the proxy, runs every command of the chosen package manager (see
/// pkg_backend.hpp for what each operation runs) via run_pkg_cli(), parses
/// the output, and pushes structured snapshots to the server-side PkgFrontend
/// form via its update_* RMI methods. Mirrors pip_source: the packages are
/// the ones on the user's own machine, reachable only from the client.
///
/// Every method that runs a command is called from a job of the shared
/// dev::command_worker, which keeps the UI responsive and shows the modal
/// progress dialog (common/command_worker.hpp).
#pragma once

#include "pkg_backend.hpp"
#include "pkg_process.hpp"

#include "modules/bdg/dev/common/command_worker.hpp"
#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::pkg {

class pkg_source {
 public:
  /// @param how  How commands that need root get it (see elevation).
  pkg_source(
      std::shared_ptr<bison::rmi::proxy::dynamic> proxy, manager m, elevation how,
      std::shared_ptr<dev::command_worker> worker);

  /// @brief Pushes the manager name, @p version_text (the probe command's
  /// first output line) and the elevation mode to the form's environment line.
  void push_environment(const std::string& version_text);

  /// @brief Pushes the installed-packages snapshot. Called once on startup,
  /// on "refresh_requested", and after every mutating action below.
  void refresh_all();

  // ── *_requested event reactions ─────────────────────────────────────────

  /// @brief Lists the packages a newer version exists for, remembers each
  /// one's latest version (shown in the Latest column from then on) and
  /// re-pushes the packages snapshot.
  void on_outdated_requested();

  /// @brief Refreshes the package index (`apt-get update`, ...), then the
  /// outdated list, which depends on it.
  void on_index_requested();

  /// @brief Upgrades every outdated package.
  void on_upgrade_all_requested();

  /// @brief Pushes one index search snapshot to update_search.
  void on_search_requested(const std::string& query);

  /// @brief Installs the whitespace-separated package names in @p text.
  void on_install_requested(const std::string& text);

  /// @brief @p action is `upgrade`, `reinstall` or `remove`.
  void on_package_action(const std::string& name, const std::string& action);

  /// @brief Pushes the verbatim output of one read-only command to
  /// update_details: @p kind `show` (the package's description) or `files`
  /// (the files it installed).
  void on_details_requested(const std::string& kind, const std::string& name);

 private:
  void push_packages();

  /// @brief Runs @p cmd (elevated when it needs root) and pushes a trace row
  /// to the Console window. Every command goes through here so the Console
  /// is a complete trace.
  process_result run_logged(const command& cmd);

  /// @brief Reports a command outcome via the form's command_result RMI
  /// method (tagged with @p scope so the right window's status label is
  /// written) and, on failure, to the progress dialog -- which @p always_show
  /// opens even for a command too quick to have opened it.
  void report(
      const std::string& label, const std::string& scope, bool ok, const std::string& output,
      bool always_show = false);

  /// @brief Runs a mutating command, refreshes, then reports its result.
  void run_and_refresh(const std::string& label, const command& cmd);

  /// @brief What a failed @p r had to say (see error_summary()).
  std::string error_text(const process_result& r) const;

  std::shared_ptr<bison::rmi::proxy::dynamic> proxy_;
  std::shared_ptr<dev::command_worker> worker_;
  manager manager_;
  elevation elevation_;

  // Latest versions from the last on_outdated_requested(), by base_name().
  std::map<std::string, std::string> latest_;
  bool outdated_checked_{false};
};

} // namespace bdg::wish::pkg
