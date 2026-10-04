// MIT License © 2026 Binary Dice Games
/// @file pkg_source.hpp
/// @brief Client-side package-manager orchestration for the pkg module.
///
/// Runs every command of the chosen package manager (see pkg_backend.hpp for
/// what each operation runs) through common::tool_source (see
/// common/tool_source.hpp), parses
/// the output, and pushes structured snapshots to the server-side PkgFrontend
/// form via its update_* RMI methods. Mirrors pip_source: the packages are
/// the ones on the user's own machine, reachable only from the client.
///
/// Every method that runs a command is called from a job of the shared
/// common::command_worker, which keeps the UI responsive and shows the modal
/// progress dialog (modules/bdg/common/command_worker.hpp).
#pragma once

#include "pkg_backend.hpp"

#include "modules/bdg/common/tool_source.hpp"

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::pkg {

using common::process_result;

class pkg_source : public common::tool_source {
 public:
  /// @param how  How commands that need root get it (see elevation).
  pkg_source(
      std::shared_ptr<bison::rmi::proxy::dynamic> proxy, manager m, elevation how,
      std::shared_ptr<common::command_worker> worker);

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

  /// @brief Runs @p cmd (elevated when it needs root) via
  /// tool_source::run_logged(), so the Console is a complete trace.
  process_result run_logged(const command& cmd);

  /// @brief Runs a mutating command, refreshes, then reports its result.
  void run_and_refresh(const std::string& label, const command& cmd);

  /// @brief What a failed @p r had to say (see error_summary()).
  std::string error_text(const process_result& r) const override;

  manager manager_;
  elevation elevation_;

  // Latest versions from the last on_outdated_requested(), by base_name().
  std::map<std::string, std::string> latest_;
  bool outdated_checked_{false};
};

} // namespace bdg::wish::pkg
