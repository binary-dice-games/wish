// MIT License © 2026 Binary Dice Games
/// @file helm_source.hpp
/// @brief Client-side `helm` command orchestration for the helm module.
///
/// Owns the proxy, runs every `helm` command via
/// helm_process::run_helm_cli(), parses the tab-separated table output (see
/// helm_table_parser.hpp), and pushes structured snapshots to the server-side
/// HelmFrontend form via its update_* RMI methods. Also reacts to the form's
/// `*_requested` events (see server/helm.hpp) by running the corresponding
/// `helm` command and refreshing.
///
/// Mirrors kubectl_source: the cluster and chart repositories a user wants to
/// manage are the ones their own `helm` CLI is configured for, reachable only
/// from the client -- the server never touches `helm` directly.
#pragma once

#include "helm_process.hpp"
#include "src/bison/bison.hpp"
#include "modules/bdg/dev/common/command_worker.hpp"
#include "src/rmi/client/proxy.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::helm {

class helm_source {
 public:
  helm_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::shared_ptr<dev::command_worker> worker);

  /// @brief Pushes the releases and repositories snapshots, in that order.
  /// Called once on startup, on "refresh_requested", and after every
  /// mutating action below.
  void refresh_all();

  // ── *_requested event reactions ─────────────────────────────────────────

  /// @brief `uninstall` -> `helm uninstall <name> -n <ns>`; `rollback` ->
  /// `helm rollback <name> [<revision>] -n <ns>` (an empty @p revision rolls
  /// back to the previous one), then re-pushes that release's history.
  void on_release_action(
      const std::string& name, const std::string& ns, const std::string& action, const std::string& revision);

  /// @brief `update` -> `helm repo update [<name>]` (an empty @p name updates
  /// every repository); `remove` -> `helm repo remove <name>`.
  void on_repo_action(const std::string& name, const std::string& action);

  /// @brief `helm repo add <name> <url>`.
  void on_repo_add(const std::string& name, const std::string& url);

  /// @brief Pushes one `helm search repo [<query>]` snapshot to update_charts
  /// (an empty @p query lists every chart of every repository). The query is
  /// remembered and re-run whenever the repositories change.
  void on_search_requested(const std::string& query);

  /// @brief Parameters of one `helm install` / `helm upgrade` run (the
  /// `install_requested` event payload).
  struct install_request {
    bool upgrade{false};
    std::string chart;
    std::string version; // "" = latest
    std::string release;
    std::string ns;
    std::string values; // YAML text passed as `-f <temp file>`; "" = none
    bool create_namespace{false};
    bool wait{false};
  };

  /// @brief `helm install|upgrade <release> <chart> -n <ns> [--version
  /// <version>] [-f <values file>] [--create-namespace] [--wait]`, reported
  /// with command_result scope "install". Non-empty values are written to an
  /// owner-only temp file for the duration of the command.
  void on_install_requested(const install_request& req);

  /// @brief Answers `install_values_requested` via set_install_values:
  /// @p source `chart` -> `helm show values <chart> [--version]`, `release`
  /// -> `helm get values <name> -n <ns> -o yaml` (the release's current
  /// user-supplied values). @p token is echoed back.
  void on_install_values_requested(
      int32_t token, const std::string& source, const std::string& chart, const std::string& version,
      const std::string& name, const std::string& ns);

  /// @brief Pushes one `helm history <name> -n <ns>` snapshot to
  /// update_history.
  void on_history_requested(const std::string& name, const std::string& ns);

  /// @brief Pushes the verbatim output of one read-only command to
  /// update_details. @p kind selects it: `status` / `values` / `manifest` /
  /// `notes` run `helm status` / `helm get <kind>` on release @p name in
  /// @p ns; `chart_values` / `chart_readme` run `helm show values|readme` on
  /// chart @p name (optionally pinned to @p version).
  void on_details_requested(
      const std::string& kind, const std::string& name, const std::string& ns, const std::string& version);

 private:
  void push_releases();
  void push_repos();

  /// @brief Runs `helm <args>` via run_helm_cli() and pushes a trace row to
  /// the Console window (HelmFrontend's append_command_log RMI method). Every
  /// `helm` invocation goes through here so the Console window is a complete
  /// trace. Mirrors kubectl_source::run_logged().
  process_result run_logged(const std::vector<std::string>& args);

  /// @brief Reports a command outcome via HelmFrontend's command_result RMI
  /// method (tagged with @p scope so the right window's status label is
  /// written). @return false when the form is gone.
  bool report(const std::string& label, const std::string& scope, bool ok, const std::string& output);

  /// @brief Runs a mutating `helm` command, reports its result and calls
  /// refresh_all(). @return whether the command succeeded.
  bool run_and_refresh(const std::string& label, const std::string& scope, const std::vector<std::string>& args);

  /// @brief Whether every value in @p values passes is_safe_arg(); reports a
  /// failure to @p scope's status label otherwise.
  bool check_args(const std::string& label, const std::string& scope, const std::vector<std::string>& values);

  std::shared_ptr<bison::rmi::proxy::dynamic> proxy_;
  // Runs every command off the UI thread, behind a modal progress dialog
  // when it takes long (see common/command_worker.hpp). Every method that
  // runs a command must be called from one of its jobs.
  std::shared_ptr<dev::command_worker> worker_;

  std::string last_query_; // the Charts window's current search
};

} // namespace bdg::wish::helm
